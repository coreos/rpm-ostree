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
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"

#include "libglnx.h"


static GOptionEntry init_option_entries[] = {
  { NULL }
};

static GOptionEntry assemble_option_entries[] = {
  { NULL }
};

typedef struct {
  char *userroot_base;
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
  rocctx->userroot_base = get_current_dir_name ();
  if (!glnx_opendirat (AT_FDCWD, rocctx->userroot_base, TRUE, &rocctx->userroot_dfd, error))
    return FALSE;

  g_autofree char *repo_pathstr = g_strconcat (rocctx->userroot_base, "/repo", NULL);
  g_autoptr(GFile) repo_path = g_file_new_for_path (repo_pathstr);
  rocctx->repo = ostree_repo_new (repo_path);

  return TRUE;
}

static gboolean
roc_context_init (ROContainerContext *rocctx,
                  GError            **error)
{
  if (!roc_context_init_core (rocctx, error))
    return FALSE;

  if (!glnx_opendirat (rocctx->userroot_dfd, "roots", TRUE, &rocctx->roots_dfd, error))
    return FALSE;

  if (!ostree_repo_open (rocctx->repo, NULL, error))
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
  rocctx->ctx = rpmostree_context_new_unprivileged (rocctx->userroot_dfd, NULL, error);
  if (!rocctx->ctx)
    return FALSE;

  if (!rpmostree_context_setup (rocctx->ctx, NULL, NULL, treespec, cancellable, error))
    return FALSE;

  return TRUE;
}

static void
roc_context_deinit (ROContainerContext *rocctx)
{
  g_free (rocctx->userroot_base);
  if (rocctx->userroot_dfd != -1)
    (void) close (rocctx->userroot_dfd);
  g_clear_object (&rocctx->repo);
  if (rocctx->roots_dfd != -1)
    (void) close (rocctx->roots_dfd);
  if (rocctx->rpmmd_dfd != -1)
    (void) close (rocctx->rpmmd_dfd);
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
                                       NULL, NULL, NULL, NULL,
                                       error))
    return EXIT_FAILURE;

  if (!roc_context_init_core (rocctx, error))
    return EXIT_FAILURE;

  static const char* const directories[] = { "repo", "rpmmd.repos.d", "cache/rpm-md", "roots", "tmp" };
  for (guint i = 0; i < G_N_ELEMENTS (directories); i++)
    {
      if (!glnx_shutil_mkdir_p_at (rocctx->userroot_dfd, directories[i], 0755, cancellable, error))
        return EXIT_FAILURE;
    }

  if (!ostree_repo_create (rocctx->repo, OSTREE_REPO_MODE_BARE_USER, cancellable, error))
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
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
                                       NULL, NULL, NULL, NULL,
                                       error))
    return EXIT_FAILURE;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "SPEC must be specified", error);
      return EXIT_FAILURE;
    }

  const char *specpath = argv[1];
  g_autoptr(RpmOstreeTreespec) treespec = rpmostree_treespec_new_from_path (specpath, error);
  if (!treespec)
    return EXIT_FAILURE;

  const char *name = rpmostree_treespec_get_ref (treespec);
  if (name == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing ref in treespec");
      return EXIT_FAILURE;
    }

  if (!roc_context_init (rocctx, error))
    return EXIT_FAILURE;

  const char *target_rootdir = glnx_strjoina (name, ".0");

  struct stat stbuf;
  if (fstatat (rocctx->roots_dfd, target_rootdir, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          return EXIT_FAILURE;
        }
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Tree %s already exists", target_rootdir);
      return EXIT_FAILURE;
    }

  if (!roc_context_prepare_for_root (rocctx, treespec, cancellable, error))
    return EXIT_FAILURE;

  /* --- Resolving dependencies --- */
  if (!rpmostree_context_prepare (rocctx->ctx, cancellable, error))
    return EXIT_FAILURE;

  rpmostree_print_transaction (rpmostree_context_get_hif (rocctx->ctx));

  /* --- Download as necessary --- */
  if (!rpmostree_context_download (rocctx->ctx, cancellable, error))
    return EXIT_FAILURE;

  /* --- Import as necessary --- */
  if (!rpmostree_context_import (rocctx->ctx, cancellable, error))
    return EXIT_FAILURE;

  g_autofree char *commit = NULL;
  { g_autofree char *tmprootfs = g_strdup ("tmp/rpmostree-commit-XXXXXX");
    glnx_fd_close int tmprootfs_dfd = -1;

    if (!glnx_mkdtempat (rocctx->userroot_dfd, tmprootfs, 0755, error))
      return EXIT_FAILURE;

    if (!glnx_opendirat (rocctx->userroot_dfd, tmprootfs, TRUE,
                         &tmprootfs_dfd, error))
      return EXIT_FAILURE;

    if (!rpmostree_context_assemble_commit (rocctx->ctx, tmprootfs_dfd, NULL,
                                            NULL, RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE,
                                            FALSE, &commit, cancellable, error))
      return EXIT_FAILURE;

    glnx_shutil_rm_rf_at (rocctx->userroot_dfd, tmprootfs, cancellable, NULL);
  }

  g_print ("Checking out %s @ %s...\n", name, commit);

  { OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                         OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

    /* Also, what we really want here is some sort of sane lifecycle
     * management with whatever is running in the root.
     */
    if (!glnx_shutil_rm_rf_at (rocctx->roots_dfd, target_rootdir, cancellable, error))
      return EXIT_FAILURE;

    if (!ostree_repo_checkout_at (rocctx->repo, &opts, rocctx->roots_dfd, target_rootdir,
                                  commit, cancellable, error))
      return EXIT_FAILURE;
  }

  g_print ("Checking out %s @ %s...done\n", name, commit);

  if (!symlink_at_replace (target_rootdir, rocctx->roots_dfd, name,
                           cancellable, error))
    return EXIT_FAILURE;

  g_print ("Creating current symlink...done\n");

  return EXIT_SUCCESS;
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
                                       NULL, NULL, NULL, NULL,
                                       error))
    return EXIT_FAILURE;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "NAME must be specified", error);
      return EXIT_FAILURE;
    }

  const char *name = argv[1];

  if (!roc_context_init (rocctx, error))
    return EXIT_FAILURE;

  g_autofree char *target_current_root = glnx_readlinkat_malloc (rocctx->roots_dfd, name, cancellable, error);
  if (!target_current_root)
    {
      g_prefix_error (error, "Reading app link %s: ", name);
      return EXIT_FAILURE;
    }

  guint current_version;
  if (!parse_app_version (target_current_root, &current_version, error))
    return EXIT_FAILURE;

  g_autofree char *commit_checksum = NULL;
  g_autofree char *previous_state_sha512 = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(RpmOstreeTreespec) treespec = NULL;
  { g_autoptr(GVariantDict) metadata_dict = NULL;
    g_autoptr(GVariant) spec_v = NULL;
    g_autoptr(GVariant) previous_sha512_v = NULL;
    g_autoptr(GVariant) commit = NULL;

    if (!ostree_repo_resolve_rev (rocctx->repo, name, FALSE, &commit_checksum, error))
      return EXIT_FAILURE;

    if (!ostree_repo_load_variant (rocctx->repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum,
                                   &commit, error))
      return EXIT_FAILURE;

    metadata = g_variant_get_child_value (commit, 0);
    metadata_dict = g_variant_dict_new (metadata);

    spec_v = _rpmostree_vardict_lookup_value_required (metadata_dict, "rpmostree.spec",
                                                                 (GVariantType*)"a{sv}", error);
    if (!spec_v)
      return EXIT_FAILURE;

    treespec = rpmostree_treespec_new (spec_v);

    previous_sha512_v = _rpmostree_vardict_lookup_value_required (metadata_dict,
                                                                  "rpmostree.state-sha512",
                                                                  (GVariantType*)"s", error);
    if (!previous_sha512_v)
      return EXIT_FAILURE;

    previous_state_sha512 = g_variant_dup_string (previous_sha512_v, NULL);
  }

  guint new_version = current_version == 0 ? 1 : 0;
  const char *target_new_root;
  if (new_version == 0)
    target_new_root = glnx_strjoina (name, ".0");
  else
    target_new_root = glnx_strjoina (name, ".1");

  if (!roc_context_prepare_for_root (rocctx, treespec, cancellable, error))
    return EXIT_FAILURE;

  if (!rpmostree_context_prepare (rocctx->ctx, cancellable, error))
    return EXIT_FAILURE;

  rpmostree_print_transaction (rpmostree_context_get_hif (rocctx->ctx));

  { g_autofree char *new_state_sha512 = rpmostree_context_get_state_sha512 (rocctx->ctx);

    if (strcmp (new_state_sha512, previous_state_sha512) == 0)
      {
        g_print ("No changes in inputs to %s (%s)\n", name, commit_checksum);
        /* Note early return */
        return EXIT_SUCCESS;
      }
  }

  /* --- Download as necessary --- */
  if (!rpmostree_context_download (rocctx->ctx, cancellable, error))
    return EXIT_FAILURE;

  /* --- Import as necessary --- */
  if (!rpmostree_context_import (rocctx->ctx, cancellable, error))
    return EXIT_FAILURE;

  g_autofree char *new_commit_checksum = NULL;
  { g_autofree char *tmprootfs = g_strdup ("tmp/rpmostree-commit-XXXXXX");
    glnx_fd_close int tmprootfs_dfd = -1;

    if (!glnx_mkdtempat (rocctx->userroot_dfd, tmprootfs, 0755, error))
      return EXIT_FAILURE;

    if (!glnx_opendirat (rocctx->userroot_dfd, tmprootfs, TRUE,
                         &tmprootfs_dfd, error))
      return EXIT_FAILURE;

    if (!rpmostree_context_assemble_commit (rocctx->ctx, tmprootfs_dfd, NULL,
                                            NULL, RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE,
                                            TRUE, &new_commit_checksum,
                                            cancellable, error))
      return EXIT_FAILURE;

    glnx_shutil_rm_rf_at (rocctx->userroot_dfd, tmprootfs, cancellable, NULL);
  }

  g_print ("Checking out %s @ %s...\n", name, new_commit_checksum);

  { OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                         OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

    if (!ostree_repo_checkout_at (rocctx->repo, &opts, rocctx->roots_dfd, target_new_root,
                                  new_commit_checksum, cancellable, error))
      return EXIT_FAILURE;
  }

  g_print ("Checking out %s @ %s...done\n", name, new_commit_checksum);

  if (!symlink_at_replace (target_new_root, rocctx->roots_dfd, name,
                           cancellable, error))
    return EXIT_FAILURE;

  g_print ("Creating current symlink...done\n");

  return EXIT_SUCCESS;
}
