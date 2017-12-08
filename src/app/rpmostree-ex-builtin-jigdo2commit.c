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
// For the jigdo Requires parsing
#include <libdnf/dnf-reldep-private.h>
#include <libdnf/dnf-sack-private.h>
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
query_jigdo_pkg (DnfContext *dnfctx,
                 const char *name,
                 const char *evr,
                 GError    **error)
{
  hy_autoquery HyQuery query = hy_query_create (dnf_context_get_sack (dnfctx));
  hy_query_filter (query, HY_PKG_NAME, HY_EQ, name);
  hy_query_filter (query, HY_PKG_EVR, HY_EQ, evr);
  g_autoptr(GPtrArray) pkglist = hy_query_run (query);
  if (pkglist->len == 0)
    return glnx_null_throw (error, "Failed to find package %s-%s", name, evr);
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

static int
compare_pkgs (gconstpointer ap,
                      gconstpointer bp)
{
  DnfPackage **a = (gpointer)ap;
  DnfPackage **b = (gpointer)bp;
  return dnf_package_cmp (*a, *b);
}

static gboolean
impl_jigdo2commit (RpmOstreeJigdo2CommitContext *self,
                   const char                   *repoid_and_oirpm_name,
                   GCancellable                 *cancellable,
                   GError                      **error)
{
  g_autofree char *oirpm_repoid = NULL;
  g_autofree char *oirpm_name = NULL;

  /* We expect REPOID:OIRPM-NAME */
  { const char *colon = strchr (repoid_and_oirpm_name, ':');
    if (!colon)
      return glnx_throw (error, "Invalid OIRPM spec '%s', expected repoid:name", repoid_and_oirpm_name);
    oirpm_repoid = g_strndup (repoid_and_oirpm_name, colon - repoid_and_oirpm_name);
    oirpm_name = g_strdup (colon + 1);
  }

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
  g_autofree char *provided_commit = NULL;
  { hy_autoquery HyQuery query = hy_query_create (dnf_context_get_sack (dnfctx));
    hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, oirpm_repoid);
    hy_query_filter (query, HY_PKG_NAME, HY_EQ, oirpm_name);
    if (opt_oirpm_version)
      hy_query_filter (query, HY_PKG_VERSION, HY_EQ, opt_oirpm_version);
    g_autoptr(GPtrArray) pkglist = hy_query_run (query);
    if (pkglist->len == 0)
      return glnx_throw (error, "Failed to find jigdo OIRPM package '%s'", oirpm_name);
    g_ptr_array_sort (pkglist, compare_pkgs_reverse);
    if (pkglist->len > 1)
      {
        g_print ("%u oirpm matches\n", pkglist->len);
      }
    g_ptr_array_set_size (pkglist, 1);
    oirpm_pkg = g_object_ref (pkglist->pdata[0]);

    /* Iterate over provides directly to provide a nicer error on mismatch */
    gboolean found_vprovide = FALSE;
    g_autoptr(DnfReldepList) provides = dnf_package_get_provides (oirpm_pkg);
    const gint n_provides = dnf_reldep_list_count (provides);
    for (int i = 0; i < n_provides; i++)
      {
        DnfReldep *provide = dnf_reldep_list_index (provides, i);

        const char *provide_str = dnf_reldep_to_string (provide);
        if (g_str_equal (provide_str, RPMOSTREE_JIGDO_PROVIDE_V2))
          {
            found_vprovide = TRUE;
          }
        else if (g_str_has_prefix (provide_str, RPMOSTREE_JIGDO_PROVIDE_COMMIT))
          {
            const char *rest = provide_str + strlen (RPMOSTREE_JIGDO_PROVIDE_COMMIT);
            if (*rest != '(')
              return glnx_throw (error, "Invalid %s", provide_str);
            rest++;
            const char *closeparen = strchr (rest, ')');
            if (!closeparen)
              return glnx_throw (error, "Invalid %s", provide_str);

            provided_commit = g_strndup (rest, closeparen - rest);
            if (strlen (provided_commit) != OSTREE_SHA256_STRING_LEN)
              return glnx_throw (error, "Invalid %s", provide_str);
          }
      }

    if (!found_vprovide)
      return glnx_throw (error, "Package '%s' does not have Provides: %s",
                         dnf_package_get_nevra (oirpm_pkg), RPMOSTREE_JIGDO_PROVIDE_V2);
    if (!provided_commit)
      return glnx_throw (error, "Package '%s' does not have Provides: %s",
                         dnf_package_get_nevra (oirpm_pkg), RPMOSTREE_JIGDO_PROVIDE_COMMIT);
  }

  g_print ("oirpm: %s (%s) commit=%s\n", dnf_package_get_nevra (oirpm_pkg),
           dnf_package_get_reponame (oirpm_pkg), provided_commit);

  { OstreeRepoCommitState commitstate;
    gboolean has_commit;
    if (!ostree_repo_has_object (self->repo, OSTREE_OBJECT_TYPE_COMMIT, provided_commit,
                                 &has_commit, cancellable, error))
      return FALSE;
    if (has_commit)
      {
        if (!ostree_repo_load_commit (self->repo, provided_commit, NULL,
                                      &commitstate, error))
          return FALSE;
        if (!(commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL))
          {
            g_print ("Commit is already written, nothing to do\n");
            return TRUE;  /* 🔚 Early return */
          }
      }
  }

  g_autoptr(GPtrArray) pkgs_required = g_ptr_array_new_with_free_func (g_object_unref);

  /* Look at the Requires of the jigdoRPM.  Note that we don't want to do
   * dependency resolution here - that's part of the whole idea, we're doing
   * deterministic imaging.
   */
  g_autoptr(DnfReldepList) requires = dnf_package_get_requires (oirpm_pkg);
  const gint n_requires = dnf_reldep_list_count (requires);
  Pool *pool = dnf_sack_get_pool (dnf_context_get_sack (dnfctx));
  for (int i = 0; i < n_requires; i++)
    {
      DnfReldep *req = dnf_reldep_list_index (requires, i);
      Id reqid = dnf_reldep_get_id (req);
      if (!ISRELDEP (reqid))
        continue;
      Reldep *rdep = GETRELDEP (pool, reqid);
      /* This is the core hack; we're searching for Requires that
       * have exact '=' versions.  This assumes that the rpmbuild
       * process won't inject such requirements.
       */
      if (!(rdep->flags & REL_EQ))
        continue;

      const char *name = pool_id2str (pool, rdep->name);
      const char *evr = pool_id2str (pool, rdep->evr);

      DnfPackage *pkg = query_jigdo_pkg (dnfctx, name, evr, error);
      // FIXME: Possibly we shouldn't require a package to be in the repos if we
      // already have it imported? This would help support downgrades if the
      // repo owner has pruned.
      if (!pkg)
        return FALSE;
      g_ptr_array_add (pkgs_required, g_object_ref (pkg));
    }
  g_ptr_array_sort (pkgs_required, compare_pkgs);

  g_print ("Jigdo from %u packages\n", pkgs_required->len);

  /* For now we first serially download the oirpm, but down the line we can do
   * this async. Doing so will require putting more of the jigdo logic into the
   * core, so it knows not to import the jigdoRPM.
   */
  { g_autoptr(GPtrArray) oirpm_singleton_pkglist = g_ptr_array_new ();
    g_ptr_array_add (oirpm_singleton_pkglist, oirpm_pkg);
    if (!rpmostree_context_set_packages (self->ctx, oirpm_singleton_pkglist, cancellable, error))
      return FALSE;
  }

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
  if (!rpmostree_jigdo_assembler_read_meta (jigdo, &checksum, &commit, &commit_meta,
                                            cancellable, error))
    return FALSE;

  if (!g_str_equal (checksum, provided_commit))
    return glnx_throw (error, "Package '%s' commit mismatch; Provides=%s, actual=%s",
                       dnf_package_get_nevra (oirpm_pkg), provided_commit, checksum);

  g_printerr ("TODO implement GPG verification\n");

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  if (!rpmostree_repo_auto_transaction_start (&txn, self->repo, FALSE, cancellable, error))
    return FALSE;

  if (!rpmostree_jigdo_assembler_write_new_objects (jigdo, self->repo, cancellable, error))
    return FALSE;

  if (!commit_and_print (self, &txn, cancellable, error))
    return FALSE;

  /* And now, process the jigdo set */
  if (!rpmostree_context_set_packages (self->ctx, pkgs_required, cancellable, error))
    return FALSE;

  /* See what packages we need to import, print their size. TODO clarify between
   * download/import.
   */
  g_autoptr(GHashTable) pkgset_to_import = g_hash_table_new (NULL, NULL);
  { g_autoptr(GPtrArray) pkgs_to_import = rpmostree_context_get_packages_to_import (self->ctx);
    guint64 dlsize = 0;
    for (guint i = 0; i < pkgs_to_import->len; i++)
      {
        DnfPackage *pkg = pkgs_to_import->pdata[i];
        dlsize += dnf_package_get_size (pkg);
        g_hash_table_add (pkgset_to_import, pkg);
      }
    g_autofree char *dlsize_fmt = g_format_size (dlsize);
    g_print ("%u packages to import, download size: %s\n", pkgs_to_import->len, dlsize_fmt);
  }

  /* Parse the xattr data in the jigdoRPM */
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

  /* Start the download and import, using the xattr data from the jigdoRPM */
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
  g_autoptr(GOptionContext) context = g_option_context_new ("REPOID:OIRPM-NAME");
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
      rpmostree_usage_error (context, "REPOID:OIRPM-NAME is required", error);
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
