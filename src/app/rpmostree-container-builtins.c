/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015,2016 Colin Walters <walters@verbum.org>
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
#include <rpm/rpmts.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-container-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"

#include "libglnx.h"


static GOptionEntry init_option_entries[] = {
  { NULL }
};

static gboolean opt_cache_only;

static GOptionEntry assemble_option_entries[] = {
  { "cache-only", 'C', 0, G_OPTION_ARG_NONE, &opt_cache_only, "Assume cache is present, do not attempt to update it", NULL },
  { NULL }
};

typedef struct {
  int userroot_dfd;

  int roots_dfd;
  OstreeRepo *repo;
  RpmOstreeContext *ctx;

  int rpmmd_dfd;
} ROContainerContext;

#define RO_CONTAINER_CONTEXT_INIT { .userroot_dfd = -1, .roots_dfd = -1, .rpmmd_dfd = -1 }

static gboolean
roc_context_init_core (ROContainerContext *rocctx,
                       GError            **error)
{
  if (!glnx_opendirat (AT_FDCWD, ".", TRUE, &rocctx->userroot_dfd, error))
    return FALSE;

  return TRUE;
}

static gboolean
roc_context_init (ROContainerContext *rocctx,
                  GError            **error)
{
  if (!roc_context_init_core (rocctx, error))
    return FALSE;

  rocctx->repo = ostree_repo_open_at (rocctx->userroot_dfd, "repo", NULL, error);
  if (!rocctx->repo)
    return FALSE;

  OstreeRepoMode mode = ostree_repo_get_mode (rocctx->repo);
  if (mode != OSTREE_REPO_MODE_BARE_USER_ONLY)
    return glnx_throw (error, "container repos are now required to be in bare-user-only mode");

  if (!glnx_opendirat (rocctx->userroot_dfd, "roots", TRUE, &rocctx->roots_dfd, error))
    return FALSE;

  if (!glnx_opendirat (rocctx->userroot_dfd, "cache/rpm-md", FALSE, &rocctx->rpmmd_dfd, error))
    return FALSE;

  return TRUE;
}

static gboolean
roc_context_prepare_for_root (ROContainerContext *rocctx,
                              RpmOstreeTreespec  *treespec,
                              GCancellable       *cancellable,
                              GError            **error)
{
  rocctx->ctx = rpmostree_context_new_tree (rocctx->userroot_dfd, rocctx->repo,
                                            cancellable, error);
  if (!rocctx->ctx)
    return FALSE;

  if (!rpmostree_context_setup (rocctx->ctx, NULL, NULL, treespec, cancellable, error))
    return FALSE;

  return TRUE;
}

static void
roc_context_deinit (ROContainerContext *rocctx)
{
  glnx_close_fd (&rocctx->userroot_dfd);
  g_clear_object (&rocctx->repo);
  glnx_close_fd (&rocctx->roots_dfd);
  glnx_close_fd (&rocctx->rpmmd_dfd);
  g_clear_object (&rocctx->ctx);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(ROContainerContext, roc_context_deinit)

int
rpmostree_container_builtin_init (int             argc,
                                  char          **argv,
                                  RpmOstreeCommandInvocation *invocation,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  g_autoptr(GOptionContext) context = g_option_context_new ("");

  if (!rpmostree_option_context_parse (context,
                                       init_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (!roc_context_init_core (rocctx, error))
    return FALSE;

  static const char* const directories[] = { "repo", "rpmmd.repos.d", "cache/rpm-md", "roots", "tmp" };
  for (guint i = 0; i < G_N_ELEMENTS (directories); i++)
    {
      if (!glnx_shutil_mkdir_p_at (rocctx->userroot_dfd, directories[i], 0755, cancellable, error))
        return FALSE;
    }

  rocctx->repo = ostree_repo_create_at (rocctx->userroot_dfd, "repo",
                                        OSTREE_REPO_MODE_BARE_USER_ONLY, NULL,
                                        cancellable, error);
  if (!rocctx->repo)
    return FALSE;

  return TRUE;
}

int
rpmostree_container_builtin_mkrootfs (int             argc,
                                      char          **argv,
                                      RpmOstreeCommandInvocation *invocation,
                                      GCancellable   *cancellable,
                                      GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("NAME [PKGNAME PKGNAME...]");
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;

  if (!rpmostree_option_context_parse (context,
                                       assemble_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "SPEC must be specified", error);
      return FALSE;
    }
  const char *specpath = argv[1];
  const char *target_rootdir = argv[2];

  g_autoptr(RpmOstreeTreespec) treespec = rpmostree_treespec_new_from_path (specpath, error);
  if (!treespec)
    return FALSE;

  if (!roc_context_init (rocctx, error))
    return FALSE;

  if (mkdirat (AT_FDCWD, target_rootdir, 0755) < 0)
    return glnx_throw_errno_prefix (error, "mkdir(%s)", target_rootdir);
  glnx_fd_close int target_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, target_rootdir, TRUE, &target_dfd, error))
    return FALSE;

  if (!roc_context_prepare_for_root (rocctx, treespec, cancellable, error))
    return FALSE;
  DnfContext *dnfctx = rpmostree_context_get_dnf (rocctx->ctx);
  if (opt_cache_only)
    dnf_context_set_cache_age (dnfctx, G_MAXUINT);

  /* --- Resolving dependencies --- */
  if (!rpmostree_context_prepare (rocctx->ctx, cancellable, error))
    return FALSE;

  rpmostree_print_transaction (rpmostree_context_get_dnf (rocctx->ctx));

  if (!rpmostree_context_download (rocctx->ctx, cancellable, error))
    return FALSE;
  if (!rpmostree_context_import (rocctx->ctx, cancellable, error))
    return FALSE;
  rpmostree_context_set_tmprootfs_dfd (rocctx->ctx, target_dfd);
  if (!rpmostree_context_assemble (rocctx->ctx, cancellable, error))
    return FALSE;
  g_print ("Generated: %s\n", target_rootdir);

  return TRUE;
}

/*
 * Like symlinkat() but overwrites (atomically) an existing
 * symlink.
 */
static gboolean
symlink_at_replace (const char    *oldpath,
                    int            parent_dfd,
                    const char    *newpath,
                    GCancellable  *cancellable,
                    GError       **error)
{
  /* Possibly in the future generate a temporary random name here,
   * would need to move "generate a temporary name" code into
   * libglnx or glib?
   */
  const char *temppath = glnx_strjoina (newpath, ".tmp");

  /* Clean up any stale temporary links */
  (void) unlinkat (parent_dfd, temppath, 0);

  /* Create the temp link */
  if (TEMP_FAILURE_RETRY (symlinkat (oldpath, parent_dfd, temppath)) < 0)
    return glnx_throw_errno_prefix (error, "symlinkat(%s)", temppath);

  /* Rename it into place */
  if (!glnx_renameat (parent_dfd, temppath, parent_dfd, newpath, error))
    return FALSE;

  return TRUE;
}

/* Download and import rpms, then generate a rootfs, and commit it */
static gboolean
download_rpms_and_assemble_commit (ROContainerContext *rocctx,
                                   char              **out_commit,
                                   GCancellable       *cancellable,
                                   GError            **error)
{
  /* --- Download as necessary --- */
  if (!rpmostree_context_download (rocctx->ctx, cancellable, error))
    return FALSE;

  /* --- Import as necessary --- */
  if (!rpmostree_context_import (rocctx->ctx, cancellable, error))
    return FALSE;

  if (!rpmostree_context_assemble (rocctx->ctx, cancellable, error))
    return FALSE;

  if (!rpmostree_rootfs_postprocess_common (rpmostree_context_get_tmprootfs_dfd (rocctx->ctx),
                                            cancellable, error))
    return FALSE;

  g_autofree char *ret_commit = NULL;
  if (!rpmostree_context_commit (rocctx->ctx, NULL, RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE,
                                 &ret_commit, cancellable, error))
    return FALSE;

  *out_commit = g_steal_pointer (&ret_commit);
  return TRUE;
}

int
rpmostree_container_builtin_assemble (int             argc,
                                      char          **argv,
                                      RpmOstreeCommandInvocation *invocation,
                                      GCancellable   *cancellable,
                                      GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("NAME [PKGNAME PKGNAME...]");
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;

  if (!rpmostree_option_context_parse (context,
                                       assemble_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "SPEC must be specified", error);
      return FALSE;
    }

  const char *specpath = argv[1];
  g_autoptr(RpmOstreeTreespec) treespec = rpmostree_treespec_new_from_path (specpath, error);
  if (!treespec)
    return FALSE;

  const char *name = rpmostree_treespec_get_ref (treespec);
  if (name == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing ref in treespec");
      return FALSE;
    }

  if (!roc_context_init (rocctx, error))
    return FALSE;

  const char *target_rootdir = glnx_strjoina (name, ".0");

  if (!glnx_fstatat_allow_noent (rocctx->roots_dfd, target_rootdir, NULL,
                                 AT_SYMLINK_NOFOLLOW, error))
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }
  if (errno == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Tree %s already exists", target_rootdir);
      return FALSE;
    }

  if (!roc_context_prepare_for_root (rocctx, treespec, cancellable, error))
    return FALSE;

  DnfContext *dnfctx = rpmostree_context_get_dnf (rocctx->ctx);
  if (opt_cache_only)
    dnf_context_set_cache_age (dnfctx, G_MAXUINT);

  /* --- Resolving dependencies --- */
  if (!rpmostree_context_prepare (rocctx->ctx, cancellable, error))
    return FALSE;

  rpmostree_print_transaction (rpmostree_context_get_dnf (rocctx->ctx));

  g_autofree char *commit = NULL;
  if (!download_rpms_and_assemble_commit (rocctx, &commit, cancellable, error))
    return FALSE;
  g_print ("Checking out %s @ %s...\n", name, commit);

  { OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                         OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

    /* Also, what we really want here is some sort of sane lifecycle
     * management with whatever is running in the root.
     */
    if (!glnx_shutil_rm_rf_at (rocctx->roots_dfd, target_rootdir, cancellable, error))
      return FALSE;

    if (!ostree_repo_checkout_at (rocctx->repo, &opts, rocctx->roots_dfd, target_rootdir,
                                  commit, cancellable, error))
      return FALSE;
  }

  g_print ("Checking out %s @ %s...done\n", name, commit);

  if (!symlink_at_replace (target_rootdir, rocctx->roots_dfd, name,
                           cancellable, error))
    return FALSE;

  g_print ("Creating current symlink...done\n");

  return TRUE;
}

#define APP_VERSION_REGEXP ".+\\.([01])"

static gboolean
parse_app_version (const char *name,
                   guint      *out_version,
                   GError    **error)
{
  g_autoptr(GMatchInfo) match = NULL;
  static gsize regex_initialized;
  static GRegex *regex;
  int ret_version;

  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new (APP_VERSION_REGEXP, 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  if (!g_regex_match (regex, name, 0, &match))
    return glnx_throw (error, "Invalid app link %s", name);

  { g_autofree char *version_str = g_match_info_fetch (match, 1);
    ret_version = g_ascii_strtoull (version_str, NULL, 10);

    switch (ret_version)
      {
      case 0:
      case 1:
        break;
      default:
        return glnx_throw (error, "Invalid version in app link %s", name);
      }
  }

  *out_version = ret_version;
  return TRUE;
}

gboolean
rpmostree_container_builtin_upgrade (int argc, char **argv,
                                     RpmOstreeCommandInvocation *invocation,
                                     GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("NAME");
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;

  if (!rpmostree_option_context_parse (context,
                                       assemble_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "NAME must be specified", error);
      return FALSE;
    }

  const char *name = argv[1];

  if (!roc_context_init (rocctx, error))
    return FALSE;

  g_autofree char *target_current_root = glnx_readlinkat_malloc (rocctx->roots_dfd, name, cancellable, error);
  if (!target_current_root)
    {
      g_prefix_error (error, "Reading app link %s: ", name);
      return FALSE;
    }

  guint current_version = 2;
  if (!parse_app_version (target_current_root, &current_version, error))
    return FALSE;
  g_assert_cmpuint (current_version, <, 2);

  g_autofree char *commit_checksum = NULL;
  g_autofree char *previous_state_sha512 = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(RpmOstreeTreespec) treespec = NULL;
  { if (!ostree_repo_resolve_rev (rocctx->repo, name, FALSE, &commit_checksum, error))
      return FALSE;

    g_autoptr(GVariant) commit = NULL;
    if (!ostree_repo_load_variant (rocctx->repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum,
                                   &commit, error))
      return FALSE;

    metadata = g_variant_get_child_value (commit, 0);
    g_autoptr(GVariantDict) metadata_dict = g_variant_dict_new (metadata);

    g_autoptr(GVariant) spec_v =
      _rpmostree_vardict_lookup_value_required (metadata_dict, "rpmostree.spec",
                                                (GVariantType*)"a{sv}", error);
    if (!spec_v)
      return FALSE;

    treespec = rpmostree_treespec_new (spec_v);
    g_autoptr(GVariant) previous_sha512_v =
      _rpmostree_vardict_lookup_value_required (metadata_dict, "rpmostree.state-sha512",
                                                (GVariantType*)"s", error);
    if (!previous_sha512_v)
      return FALSE;

    previous_state_sha512 = g_variant_dup_string (previous_sha512_v, NULL);
  }

  guint new_version = (current_version == 0 ? 1 : 0);
  const char *target_new_root;
  if (new_version == 0)
    target_new_root = glnx_strjoina (name, ".0");
  else
    target_new_root = glnx_strjoina (name, ".1");

  if (!roc_context_prepare_for_root (rocctx, treespec, cancellable, error))
    return FALSE;

  if (!rpmostree_context_prepare (rocctx->ctx, cancellable, error))
    return FALSE;

  rpmostree_print_transaction (rpmostree_context_get_dnf (rocctx->ctx));

  {
    g_autofree char *new_state_sha512 = NULL;
    if (!rpmostree_context_get_state_sha512 (rocctx->ctx, &new_state_sha512, error))
      return FALSE;

    if (strcmp (new_state_sha512, previous_state_sha512) == 0)
      {
        g_print ("No changes in inputs to %s (%s)\n", name, commit_checksum);
        /* Note early return */
        return TRUE;
      }
  }

  g_autofree char *new_commit_checksum = NULL;
  if (!download_rpms_and_assemble_commit (rocctx, &new_commit_checksum, cancellable, error))
    return FALSE;

  g_print ("Checking out %s @ %s...\n", name, new_commit_checksum);

  { OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                         OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

    if (!ostree_repo_checkout_at (rocctx->repo, &opts, rocctx->roots_dfd, target_new_root,
                                  new_commit_checksum, cancellable, error))
      return FALSE;
  }

  g_print ("Checking out %s @ %s...done\n", name, new_commit_checksum);

  if (!symlink_at_replace (target_new_root, rocctx->roots_dfd, name,
                           cancellable, error))
    return FALSE;

  g_print ("Creating current symlink...done\n");

  return TRUE;
}
