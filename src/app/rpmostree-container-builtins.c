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
#include "rpmostree-bwrap.h"
#include "rpmostree-core.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"

#include "libglnx.h"


static GOptionEntry init_option_entries[] = {
  { NULL }
};

static char *opt_postprocess_from_host;
static gboolean opt_postprocess_networking;

static GOptionEntry assemble_option_entries[] = {
  { "postprocess-from-host", 0, 0, G_OPTION_ARG_STRING, &opt_postprocess_from_host, "Execute SCRIPT in a container with target mounted at /target-rootfs", "SCRIPT" },
  { "postprocess-networking", 0, 0, G_OPTION_ARG_NONE, &opt_postprocess_networking, "Enable networking (Share network namespace in container)", NULL },
  { NULL }
};

typedef struct {
  char *userroot_base;
  int userroot_dfd;

  int roots_dfd;
  OstreeRepo *repo;
  RpmOstreeContext *ctx;

  int tmprootfs_dfd;
  char *tmprootfs;

  OstreeRepoDevInoCache *devino_cache;

  int rpmmd_dfd;
} ROContainerContext;

#define RO_CONTAINER_CONTEXT_INIT { .userroot_dfd = -1, .rpmmd_dfd = -1 }

static gboolean
roc_context_init_core (ROContainerContext *rocctx,
                       GError            **error)
{
  gboolean ret = FALSE;

  rocctx->userroot_base = get_current_dir_name ();
  if (!glnx_opendirat (AT_FDCWD, rocctx->userroot_base, TRUE, &rocctx->userroot_dfd, error))
    goto out;

  { g_autofree char *repo_pathstr = g_strconcat (rocctx->userroot_base, "/repo", NULL);
    g_autoptr(GFile) repo_path = g_file_new_for_path (repo_pathstr);
    rocctx->repo = ostree_repo_new (repo_path);
  }

  rocctx->devino_cache = ostree_repo_devino_cache_new ();
  rocctx->tmprootfs_dfd = -1;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
roc_context_init (ROContainerContext *rocctx,
                  GError            **error)
{
  gboolean ret = FALSE;
  
  if (!roc_context_init_core (rocctx, error))
    goto out;

  if (!glnx_opendirat (rocctx->userroot_dfd, "roots", TRUE, &rocctx->roots_dfd, error))
    goto out;

  if (!ostree_repo_open (rocctx->repo, NULL, error))
    goto out;

  if (!glnx_opendirat (rocctx->userroot_dfd, "cache/rpm-md", FALSE, &rocctx->rpmmd_dfd, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
roc_context_prepare_for_root (ROContainerContext *rocctx,
                              RpmOstreeTreespec  *treespec,
                              GCancellable       *cancellable,
                              GError            **error)
{
  gboolean ret = FALSE;

  rocctx->ctx = rpmostree_context_new_unprivileged (rocctx->userroot_dfd, NULL, error);
  if (!rocctx->ctx)
    goto out;

  if (!rpmostree_context_setup (rocctx->ctx, NULL, "/", treespec, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static void
roc_context_deinit (ROContainerContext *rocctx)
{
  g_free (rocctx->userroot_base);
  if (rocctx->userroot_dfd)
    (void) close (rocctx->userroot_dfd);
  g_clear_object (&rocctx->repo);
  if (rocctx->roots_dfd)
    (void) close (rocctx->roots_dfd);
  if (rocctx->rpmmd_dfd)
    (void) close (rocctx->rpmmd_dfd);
  if (rocctx->devino_cache)
    ostree_repo_devino_cache_unref (rocctx->devino_cache);
  g_clear_object (&rocctx->ctx);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(ROContainerContext, roc_context_deinit)

int
rpmostree_container_builtin_init (int             argc,
                                  char          **argv,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  int exit_status = EXIT_FAILURE;
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  static const char* const directories[] = { "repo", "rpmmd.repos.d", "cache/rpm-md", "roots", "tmp" };
  guint i;
  
  if (!rpmostree_option_context_parse (context,
                                       init_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (!roc_context_init_core (rocctx, error))
    goto out;

  for (i = 0; i < G_N_ELEMENTS (directories); i++)
    {
      if (!glnx_shutil_mkdir_p_at (rocctx->userroot_dfd, directories[i], 0755, cancellable, error))
        goto out;
    }

  if (!ostree_repo_create (rocctx->repo, OSTREE_REPO_MODE_BARE_USER, cancellable, error))
    goto out;

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
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
  gboolean ret = FALSE;
  int res;
  /* Possibly in the future generate a temporary random name here,
   * would need to move "generate a temporary name" code into
   * libglnx or glib?
   */
  const char *temppath = glnx_strjoina (newpath, ".tmp");

  /* Clean up any stale temporary links */ 
  (void) unlinkat (parent_dfd, temppath, 0);

  /* Create the temp link */ 
  do
    res = symlinkat (oldpath, parent_dfd, temppath);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* Rename it into place */ 
  do
    res = renameat (parent_dfd, temppath, parent_dfd, newpath);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
prepare_assemble_import (ROContainerContext *rocctx,
                         RpmOstreeTreespec  *treespec,
                         GCancellable       *cancellable,
                         GError            **error)
{
  g_autoptr(RpmOstreeInstall) install = {0,};

  if (!roc_context_prepare_for_root (rocctx, treespec, cancellable, error))
    return FALSE;

  /* --- Downloading metadata --- */
  if (!rpmostree_context_download_metadata (rocctx->ctx, cancellable, error))
    return FALSE;

  /* --- Resolving dependencies --- */
  if (!rpmostree_context_prepare_install (rocctx->ctx, &install, cancellable, error))
    return FALSE;

  rpmostree_print_transaction (rpmostree_context_get_hif (rocctx->ctx));

  /* --- Download as necessary --- */
  if (!rpmostree_context_download (rocctx->ctx, install, cancellable, error))
    return FALSE;

  /* --- Import as necessary --- */
  if (!rpmostree_context_import (rocctx->ctx, install, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
open_tmprootfs (ROContainerContext *rocctx,
                GError **error)
{
  g_autofree char *tmprootfs = g_strdup ("tmp/rpmostree-commit-XXXXXX");

  if (!glnx_mkdtempat (rocctx->userroot_dfd, tmprootfs, 0755, error))
    return FALSE;

  rocctx->tmprootfs = g_steal_pointer (&tmprootfs);

  if (!glnx_opendirat (rocctx->userroot_dfd, rocctx->tmprootfs, TRUE,
                       &rocctx->tmprootfs_dfd, error))
    return FALSE;

  return TRUE;
}

int
rpmostree_container_builtin_assemble_export (int             argc,
                                             char          **argv,
                                             GCancellable   *cancellable,
                                             GError        **error)
{
  int exit_status = EXIT_FAILURE;
  g_autoptr(GOptionContext) context = g_option_context_new ("TREESPEC");
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  const char *specpath;
  const char *name;
  g_autofree char *commit = NULL;
  g_autoptr(RpmOstreeTreespec) treespec = NULL;

  if (!rpmostree_option_context_parse (context,
                                       assemble_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "TREESPEC must be specified", error);
      goto out;
    }

  specpath = argv[1];
  treespec = rpmostree_treespec_new_from_path (specpath, error);
  if (!treespec)
    goto out;

  name = rpmostree_treespec_get_ref (treespec);
  if (name == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing ref in treespec");
      goto out;
    }

  if (!roc_context_init (rocctx, error))
    goto out;

  if (!prepare_assemble_import (rocctx, treespec, cancellable, error))
    goto out;

  if (!open_tmprootfs (rocctx, error))
    goto out;

  if (!rpmostree_context_assemble_tmprootfs (rocctx->ctx, rocctx->tmprootfs_dfd,
                                             rocctx->devino_cache,
                                             RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE,
                                             FALSE, cancellable, error))
    goto out;

  if (opt_postprocess_from_host)
    {
      g_autoptr(RpmOstreeBwrap) bwrap = rpmostree_bwrap_new_host (error, NULL);
      if (!opt_postprocess_networking)
        rpmostree_bwrap_append_bwrap_argv (bwrap, "--unshare-net", NULL);

      rpmostree_bwrap_append_bwrap_argv (bwrap, "--tmpfs", "/tmp", NULL);
      if (!rpmostree_bwrap_bind_rofiles (bwrap, rocctx->tmprootfs_dfd, ".", "/tmp/target-rootfs", error))
        goto out;

      rpmostree_bwrap_append_child_argv (bwrap, opt_postprocess_from_host, NULL);

      if (!rpmostree_bwrap_run (bwrap, error))
        goto out;
    }

  if (!rpmostree_context_commit_tmprootfs (rocctx->ctx, rocctx->tmprootfs_dfd,
                                           rocctx->devino_cache,
                                           NULL,
                                           RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE,
                                           &commit, cancellable, error))
    goto out;

  exit_status = EXIT_SUCCESS;
 out:
  if (rocctx->tmprootfs)
    (void) glnx_shutil_rm_rf_at (rocctx->userroot_dfd, rocctx->tmprootfs, NULL, NULL);
  return exit_status;
}

int
rpmostree_container_builtin_assemble_checkout (int             argc,
                                               char          **argv,
                                               GCancellable   *cancellable,
                                               GError        **error)
{
  int exit_status = EXIT_FAILURE;
  g_autoptr(GOptionContext) context = g_option_context_new ("TREESPEC");
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  const char *specpath;
  struct stat stbuf;
  const char *name;
  g_autofree char *commit = NULL;
  const char *target_rootdir;
  g_autoptr(RpmOstreeTreespec) treespec = NULL;
  
  if (!rpmostree_option_context_parse (context,
                                       assemble_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "TREESPEC must be specified", error);
      goto out;
    }

  specpath = argv[1];
  treespec = rpmostree_treespec_new_from_path (specpath, error);
  if (!treespec)
    goto out;

  name = rpmostree_treespec_get_ref (treespec);
  if (name == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing ref in treespec");
      goto out;
    }

  if (!roc_context_init (rocctx, error))
    goto out;

  target_rootdir = glnx_strjoina (name, ".0");

  if (fstatat (rocctx->roots_dfd, target_rootdir, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Tree %s already exists", target_rootdir);
      goto out;
    }

  if (!prepare_assemble_import (rocctx, treespec, cancellable, error))
    goto out;

  if (!open_tmprootfs (rocctx, error))
    goto out;

  if (!rpmostree_context_assemble_commit (rocctx->ctx, rocctx->tmprootfs_dfd, NULL,
                                          NULL, RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE,
                                          FALSE, &commit, cancellable, error))
    goto out;

  (void) close (glnx_steal_fd (&rocctx->tmprootfs_dfd));
  glnx_shutil_rm_rf_at (rocctx->userroot_dfd, rocctx->tmprootfs, cancellable, NULL);
  g_free (g_steal_pointer (&rocctx->tmprootfs));

  g_print ("Checking out %s @ %s...\n", name, commit);

  { OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                         OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

    /* Also, what we really want here is some sort of sane lifecycle
     * management with whatever is running in the root.
     */
    if (!glnx_shutil_rm_rf_at (rocctx->roots_dfd, target_rootdir, cancellable, error))
      goto out;

    if (!ostree_repo_checkout_at (rocctx->repo, &opts, rocctx->roots_dfd, target_rootdir,
                                  commit, cancellable, error))
      goto out;
  }

  g_print ("Checking out %s @ %s...done\n", name, commit);

  if (!symlink_at_replace (target_rootdir, rocctx->roots_dfd, name,
                           cancellable, error))
    goto out;

  g_print ("Creating current symlink...done\n");

  exit_status = EXIT_SUCCESS;
 out:
  if (rocctx->tmprootfs_dfd != -1)
    (void) close (glnx_steal_fd (&rocctx->tmprootfs_dfd));
  if (rocctx->tmprootfs)
    (void) glnx_shutil_rm_rf_at (rocctx->userroot_dfd, rocctx->tmprootfs, NULL, NULL);
  return exit_status;
}

#define APP_VERSION_REGEXP ".+\\.([01])"

static gboolean
parse_app_version (const char *name,
                   guint      *out_version,
                   GError    **error)
{
  gboolean ret = FALSE;
  GMatchInfo *match = NULL;
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
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid app link %s", name);
      goto out;
    }

  { g_autofree char *version_str = g_match_info_fetch (match, 1);
    ret_version = g_ascii_strtoull (version_str, NULL, 10);
    
    switch (ret_version)
      {
      case 0:
      case 1:
        break;
      default:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Invalid version in app link %s", name);
        goto out;
      }
  }

  ret = TRUE;
  *out_version = ret_version;
 out:
  return ret;
}

gboolean
rpmostree_container_builtin_upgrade (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  int exit_status = EXIT_FAILURE;
  g_autoptr(GOptionContext) context = g_option_context_new ("NAME");
  g_auto(ROContainerContext) rocctx_data = RO_CONTAINER_CONTEXT_INIT;
  ROContainerContext *rocctx = &rocctx_data;
  g_autoptr(RpmOstreeInstall) install = NULL;
  const char *name;
  g_autofree char *commit_checksum = NULL;
  g_autofree char *new_commit_checksum = NULL;
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(RpmOstreeTreespec) treespec = NULL;
  guint current_version;
  guint new_version;
  g_autofree char *previous_state_sha512 = NULL;
  const char *target_current_root;
  const char *target_new_root;
  
  if (!rpmostree_option_context_parse (context,
                                       assemble_option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (argc < 1)
    {
      rpmostree_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  name = argv[1];

  if (!roc_context_init (rocctx, error))
    goto out;

  target_current_root = glnx_readlinkat_malloc (rocctx->roots_dfd, name, cancellable, error);
  if (!target_current_root)
    {
      g_prefix_error (error, "Reading app link %s: ", name);
      goto out;
    }

  if (!parse_app_version (target_current_root, &current_version, error))
    goto out;

  { g_autoptr(GVariantDict) metadata_dict = NULL;
    g_autoptr(GVariant) spec_v = NULL;
    g_autoptr(GVariant) previous_sha512_v = NULL;

    if (!ostree_repo_resolve_rev (rocctx->repo, name, FALSE, &commit_checksum, error))
      goto out;

    if (!ostree_repo_load_variant (rocctx->repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum,
                                   &commit, error))
      goto out;

    metadata = g_variant_get_child_value (commit, 0);
    metadata_dict = g_variant_dict_new (metadata);

    spec_v = _rpmostree_vardict_lookup_value_required (metadata_dict, "rpmostree.spec",
                                                                 (GVariantType*)"a{sv}", error);
    if (!spec_v)
      goto out;

    treespec = rpmostree_treespec_new (spec_v);

    previous_sha512_v = _rpmostree_vardict_lookup_value_required (metadata_dict,
                                                                  "rpmostree.state-sha512",
                                                                  (GVariantType*)"s", error);
    if (!previous_sha512_v)
      goto out;

    previous_state_sha512 = g_variant_dup_string (previous_sha512_v, NULL);
  }

  new_version = current_version == 0 ? 1 : 0;
  if (new_version == 0)
    target_new_root = glnx_strjoina (name, ".0");
  else
    target_new_root = glnx_strjoina (name, ".1");

  if (!roc_context_prepare_for_root (rocctx, treespec, cancellable, error))
    goto out;

  /* --- Downloading metadata --- */
  if (!rpmostree_context_download_metadata (rocctx->ctx, cancellable, error))
    goto out;

  /* --- Resolving dependencies --- */
  if (!rpmostree_context_prepare_install (rocctx->ctx, &install,
                                          cancellable, error))
    goto out;

  rpmostree_print_transaction (rpmostree_context_get_hif (rocctx->ctx));

  { g_autofree char *new_state_sha512 = rpmostree_context_get_state_sha512 (rocctx->ctx);

    if (strcmp (new_state_sha512, previous_state_sha512) == 0)
      {
        g_print ("No changes in inputs to %s (%s)\n", name, commit_checksum);
        exit_status = EXIT_SUCCESS;
        goto out;
      }
  }

  /* --- Download as necessary --- */
  if (!rpmostree_context_download (rocctx->ctx, install, cancellable, error))
    goto out;

  /* --- Import as necessary --- */
  if (!rpmostree_context_import (rocctx->ctx, install, cancellable, error))
    goto out;

  { g_autofree char *tmprootfs = g_strdup ("tmp/rpmostree-commit-XXXXXX");
    glnx_fd_close int tmprootfs_dfd = -1;

    if (!glnx_mkdtempat (rocctx->userroot_dfd, tmprootfs, 0755, error))
      goto out;

    if (!glnx_opendirat (rocctx->userroot_dfd, tmprootfs, TRUE,
                         &tmprootfs_dfd, error))
      goto out;

    if (!rpmostree_context_assemble_commit (rocctx->ctx, tmprootfs_dfd, NULL,
                                            NULL, RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE,
                                            TRUE, &new_commit_checksum,
                                            cancellable, error))
      goto out;

    glnx_shutil_rm_rf_at (rocctx->userroot_dfd, tmprootfs, cancellable, NULL);
  }

  g_print ("Checking out %s @ %s...\n", name, new_commit_checksum);

  { OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                         OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

    if (!ostree_repo_checkout_at (rocctx->repo, &opts, rocctx->roots_dfd, target_new_root,
                                  new_commit_checksum, cancellable, error))
      goto out;
  }

  g_print ("Checking out %s @ %s...done\n", name, new_commit_checksum);

  if (!symlink_at_replace (target_new_root, rocctx->roots_dfd, name,
                           cancellable, error))
    goto out;

  g_print ("Creating current symlink...done\n");

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
