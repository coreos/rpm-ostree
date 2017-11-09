/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <glib-unix.h>
#include <gio/gunixoutputstream.h>
#include <libdnf/libdnf.h>
#include <sys/mount.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-ex-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-jigdo-assembler.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"

#include "libglnx.h"

static char *opt_repo;
static char *opt_rpmmd_reposdir;
static char *opt_releasever;
static char **opt_enable_rpmmdrepo;
static char *opt_oirpm_version;

static GOptionEntry jigdo2commit_option_entries[] = {
  { "repo", 0, 0, G_OPTION_ARG_STRING, &opt_repo, "OSTree repo", "REPO" },
  { "rpmmd-reposd", 'd', 0, G_OPTION_ARG_STRING, &opt_rpmmd_reposdir, "Path to yum.repos.d (rpmmd) config directory", "PATH" },
  { "enablerepo", 'e', 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_rpmmdrepo, "Enable rpm-md repo with id ID", "ID" },
  { "releasever", 0, 0, G_OPTION_ARG_STRING, &opt_releasever, "Value for $releasever", "RELEASEVER" },
  { "oirpm-version", 'V', 0, G_OPTION_ARG_STRING, &opt_oirpm_version, "Use this specific version of OIRPM", "VERSION" },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
  GLnxTmpDir tmpd;
  RpmOstreeContext *ctx;
} RpmOstreeJigdo2CommitContext;

static void
rpm_ostree_jigdo2commit_context_free (RpmOstreeJigdo2CommitContext *ctx)
{
  g_clear_object (&ctx->repo);
  (void) glnx_tmpdir_delete (&ctx->tmpd, NULL, NULL);
  g_free (ctx);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeJigdo2CommitContext, rpm_ostree_jigdo2commit_context_free)

/* Initialize a context for converting a jigdo to a commit.
 */
static gboolean
rpm_ostree_jigdo2commit_context_new (RpmOstreeJigdo2CommitContext **out_context,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
  g_autoptr(RpmOstreeJigdo2CommitContext) self = g_new0 (RpmOstreeJigdo2CommitContext, 1);

  self->repo = ostree_repo_open_at (AT_FDCWD, opt_repo, cancellable, error);
  if (!self->repo)
    return FALSE;

  /* Our workdir lives in the repo for command line testing */
  if (!glnx_mkdtempat (ostree_repo_get_dfd (self->repo),
                       "tmp/rpmostree-jigdo-XXXXXX", 0700, &self->tmpd, error))
    return FALSE;

  self->ctx = rpmostree_context_new_tree (self->tmpd.fd, self->repo, cancellable, error);
  if (!self->ctx)
    return FALSE;

  DnfContext *dnfctx = rpmostree_context_get_dnf (self->ctx);

  if (opt_rpmmd_reposdir)
    dnf_context_set_repo_dir (dnfctx, opt_rpmmd_reposdir);

  *out_context = g_steal_pointer (&self);
  return TRUE;
}

static DnfPackage *
query_nevra (DnfContext *dnfctx,
             const char *name,
             guint64     epoch,
             const char *version,
             const char *release,
             const char *arch,
             GError    **error)
{
  hy_autoquery HyQuery query = hy_query_create (dnf_context_get_sack (dnfctx));
  hy_query_filter (query, HY_PKG_NAME, HY_EQ, name);
  hy_query_filter_num (query, HY_PKG_EPOCH, HY_EQ, epoch);
  hy_query_filter (query, HY_PKG_VERSION, HY_EQ, version);
  hy_query_filter (query, HY_PKG_RELEASE, HY_EQ, release);
  hy_query_filter (query, HY_PKG_ARCH, HY_EQ, arch);
  g_autoptr(GPtrArray) pkglist = hy_query_run (query);
  if (pkglist->len == 0)
    return glnx_null_throw (error, "Failed to find package '%s'", name);
  return g_object_ref (pkglist->pdata[0]);
}

static gboolean
commit_and_print (RpmOstreeJigdo2CommitContext *self,
                  RpmOstreeRepoAutoTransaction *txn,
                  GCancellable                 *cancellable,
                  GError                      **error)
{
  OstreeRepoTransactionStats stats;
  if (!ostree_repo_commit_transaction (self->repo, &stats, cancellable, error))
    return FALSE;
  txn->initialized = FALSE;

  g_print ("Metadata Total: %u\n", stats.metadata_objects_total);
  g_print ("Metadata Written: %u\n", stats.metadata_objects_written);
  g_print ("Content Total: %u\n", stats.content_objects_total);
  g_print ("Content Written: %u\n", stats.content_objects_written);
  g_print ("Content Bytes Written: %" G_GUINT64_FORMAT "\n", stats.content_bytes_written);

  return TRUE;
}

static int
compare_pkgs_reverse (gconstpointer ap,
                      gconstpointer bp)
{
  DnfPackage **a = (gpointer)ap;
  DnfPackage **b = (gpointer)bp;
  return dnf_package_cmp (*b, *a); // Reverse
}

static gboolean
impl_jigdo2commit (RpmOstreeJigdo2CommitContext *self,
                   const char                   *oirpm_name,
                   GCancellable                 *cancellable,
                   GError                      **error)
{
  g_autoptr(GKeyFile) tsk = g_key_file_new ();

  if (opt_releasever)
    g_key_file_set_string (tsk, "tree", "releasever", opt_releasever);
  if (opt_enable_rpmmdrepo)
    g_key_file_set_string_list (tsk, "tree", "repos",
                                (const char *const*)opt_enable_rpmmdrepo,
                                g_strv_length (opt_enable_rpmmdrepo));
  g_autoptr(RpmOstreeTreespec) treespec = rpmostree_treespec_new_from_keyfile (tsk, error);
  if (!treespec)
    return FALSE;


  if (!rpmostree_context_setup (self->ctx, NULL, NULL, treespec, cancellable, error))
    return FALSE;
  if (!rpmostree_context_download_metadata (self->ctx, cancellable, error))
    return FALSE;

  DnfContext *dnfctx = rpmostree_context_get_dnf (self->ctx);
  g_autoptr(DnfPackage) oirpm_pkg = NULL;
  { hy_autoquery HyQuery query = hy_query_create (dnf_context_get_sack (dnfctx));
    if (opt_oirpm_version)
      {
        hy_query_filter (query, HY_PKG_NAME, HY_EQ, oirpm_name);
        hy_query_filter (query, HY_PKG_VERSION, HY_EQ, opt_oirpm_version);
      }
    else
      {
        hy_query_filter (query, HY_PKG_NAME, HY_EQ, oirpm_name);
      }
    g_autoptr(GPtrArray) pkglist = hy_query_run (query);
    if (pkglist->len == 0)
      return glnx_throw (error, "Failed to find jigdo OIRPM package '%s'", oirpm_name);
    g_ptr_array_sort (pkglist, compare_pkgs_reverse);
    if (pkglist->len > 1)
      {
        g_print ("%u oirpm matches\n", pkglist->len);
      }
    g_ptr_array_set_size (pkglist, 1);
    if (!rpmostree_context_set_packages (self->ctx, pkglist, cancellable, error))
      return FALSE;
    oirpm_pkg = g_object_ref (pkglist->pdata[0]);
  }

  g_print ("oirpm: %s (%s)\n", dnf_package_get_nevra (oirpm_pkg),
           dnf_package_get_reponame (oirpm_pkg));

  if (!rpmostree_context_download (self->ctx, cancellable, error))
    return FALSE;

  glnx_fd_close int oirpm_fd = -1;
  if (!rpmostree_context_consume_package (self->ctx, oirpm_pkg, &oirpm_fd, error))
    return FALSE;

  g_autoptr(RpmOstreeJigdoAssembler) jigdo = rpmostree_jigdo_assembler_new_take_fd (&oirpm_fd, oirpm_pkg, error);
  if (!jigdo)
    return FALSE;
  g_autofree char *checksum = NULL;
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) commit_meta = NULL;
  g_autoptr(GVariant) pkgs = NULL;
  if (!rpmostree_jigdo_assembler_read_meta (jigdo, &checksum, &commit, &commit_meta, &pkgs,
                                  cancellable, error))
    return FALSE;

  g_print ("OSTree commit: %s\n", checksum);

  { OstreeRepoCommitState commitstate;
    gboolean has_commit;
    if (!ostree_repo_has_object (self->repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &has_commit, cancellable, error))
      return FALSE;
    if (has_commit)
      {
        if (!ostree_repo_load_commit (self->repo, checksum, NULL, &commitstate, error))
          return FALSE;
        if (!(commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL))
          {
            g_print ("Commit is already written, nothing to do\n");
            return TRUE;  /* 🔚 Early return */
          }
      }
  }

  g_printerr ("TODO implement GPG verification\n");

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  if (!rpmostree_repo_auto_transaction_start (&txn, self->repo, FALSE, cancellable, error))
    return FALSE;

  if (!rpmostree_jigdo_assembler_write_new_objects (jigdo, self->repo, cancellable, error))
    return FALSE;

  if (!commit_and_print (self, &txn, cancellable, error))
    return FALSE;

  /* Download any packages we don't already have imported */
  g_autoptr(GPtrArray) pkgs_required = g_ptr_array_new_with_free_func (g_object_unref);
  const guint n_pkgs = g_variant_n_children (pkgs);
  for (guint i = 0; i < n_pkgs; i++)
    {
      const char *name, *version, *release, *architecture;
      const char *repodata_checksum;
      guint64 epoch;
      g_variant_get_child (pkgs, i, "(&st&s&s&s&s)",
                           &name, &epoch, &version, &release, &architecture,
                           &repodata_checksum);
      // TODO: use repodata checksum, but probably only if covered by the ostree
      // gpg sig?
      DnfPackage *pkg = query_nevra (dnfctx, name, epoch, version, release, architecture, error);
      if (!pkg)
        return FALSE;
      g_ptr_array_add (pkgs_required, g_object_ref (pkg));
    }

  g_print ("Jigdo from %u packages\n", pkgs_required->len);

  if (!rpmostree_context_set_packages (self->ctx, pkgs_required, cancellable, error))
    return FALSE;

  g_autoptr(GHashTable) pkgset_to_import = g_hash_table_new (NULL, NULL);
  { g_autoptr(GPtrArray) pkgs_to_import = rpmostree_context_get_packages_to_import (self->ctx);
    for (guint i = 0; i < pkgs_to_import->len; i++)
      g_hash_table_add (pkgset_to_import, pkgs_to_import->pdata[i]);
    g_print ("%u packages to import\n", pkgs_to_import->len);
  }

  g_autoptr(GHashTable) pkg_to_xattrs = g_hash_table_new_full (NULL, NULL,
                                                               (GDestroyNotify)g_object_unref,
                                                               (GDestroyNotify)g_variant_unref);

  for (guint i = 0; i < pkgs_required->len; i++)
    {
      DnfPackage *pkg = pkgs_required->pdata[i];
      const gboolean should_import = g_hash_table_contains (pkgset_to_import, pkg);
      g_autoptr(GVariant) objid_to_xattrs = NULL;
      if (!rpmostree_jigdo_assembler_next_xattrs (jigdo, &objid_to_xattrs, cancellable, error))
        return FALSE;
      if (!objid_to_xattrs)
        return glnx_throw (error, "missing xattr entry: %s", dnf_package_get_name (pkg));
      if (!should_import)
        continue;
      g_hash_table_insert (pkg_to_xattrs, g_object_ref (pkg), g_steal_pointer (&objid_to_xattrs));
    }

  if (!rpmostree_context_download (self->ctx, cancellable, error))
    return FALSE;
  g_autoptr(GVariant) xattr_table = rpmostree_jigdo_assembler_get_xattr_table (jigdo);
  if (!rpmostree_context_import_jigdo (self->ctx, xattr_table, pkg_to_xattrs,
                                       cancellable, error))
    return FALSE;

  /* Write commitmeta/commit last since libostree doesn't expose an API to set
   * partial state right now.
   */
  if (!ostree_repo_write_commit_detached_metadata (self->repo, checksum, commit_meta,
                                                   cancellable, error))
    return FALSE;
  { g_autofree guint8*csum = NULL;
    if (!ostree_repo_write_metadata (self->repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     checksum, commit, &csum,
                                     cancellable, error))
      return FALSE;
  }

  return TRUE;
}

int
rpmostree_ex_builtin_jigdo2commit (int             argc,
                                   char          **argv,
                                   RpmOstreeCommandInvocation *invocation,
                                   GCancellable   *cancellable,
                                   GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("OIRPM");
  if (!rpmostree_option_context_parse (context,
                                       jigdo2commit_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL,
                                       error))
    return EXIT_FAILURE;

  if (argc != 2)
   {
      rpmostree_usage_error (context, "OIRPM name is required", error);
      return EXIT_FAILURE;
    }

  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      return EXIT_FAILURE;
    }

  const char *oirpm = argv[1];

  g_autoptr(RpmOstreeJigdo2CommitContext) self = NULL;
  if (!rpm_ostree_jigdo2commit_context_new (&self, cancellable, error))
    return EXIT_FAILURE;
  if (!impl_jigdo2commit (self, oirpm, cancellable, error))
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
}
