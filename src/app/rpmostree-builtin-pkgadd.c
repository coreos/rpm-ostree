/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmdb.h>
#include <libhif.h>
#include <libhif/hif-utils.h>

#include "rpmostree-builtins.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-hif.h"
#include "rpmostree-rpm-util.h"

#include "libgsystem.h"

static char *opt_sysroot = "/";
static char *opt_osname;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { NULL }
};

static gboolean
instroot_make_rpmdb_copy (const char     *root,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  gs_free char *rpmdb_path = g_build_filename (root, "usr/share/rpm", NULL);
  gs_free char *rpmdb_path_tmp = g_build_filename (root, "usr/share/rpm.tmp", NULL);

  if (rename (rpmdb_path, rpmdb_path_tmp) != 0)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }

  /* Now make a full copy - otherwise we risk corrupting the object store. */
  { gs_unref_object GFile *src = g_file_new_for_path (rpmdb_path_tmp);
    gs_unref_object GFile *dest = g_file_new_for_path (rpmdb_path);

    if (!gs_shutil_cp_a (src, dest, cancellable, error))
      goto out;

    if (!gs_shutil_rm_rf (src, cancellable, error))
      goto out;
  }

  ret = TRUE;
 out:
  return ret;
}

static void
on_hifstate_percentage_changed (HifState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  glnx_console_progress_text_percent (text, percentage);
}

static gboolean
overlay_packages_in_deploydir (HifContext      *hifctx,
                               const char      *deploydir,
                               GCancellable    *cancellable,
                               GError         **error)
{
  gboolean ret = FALSE;

  if (!instroot_make_rpmdb_copy (deploydir, cancellable, error))
    goto out;
  
  /* --- Run transaction --- */
  { g_auto(GLnxConsoleRef) console = { 0, };
    gs_unref_object HifState *hifstate = hif_state_new ();
    guint progress_sigid;
    
    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                       G_CALLBACK (on_hifstate_percentage_changed), 
                                       "Installing: ");
    
    glnx_console_lock (&console);

    if (!hif_context_commit (hifctx, hifstate, error))
      goto out;

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_builtin_pkgadd (int             argc,
                          char          **argv,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("- Add packages to the desired system state");
  gs_unref_object GFile *sysroot_path = NULL;
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  OstreeDeployment *booted_deployment = NULL;
  gs_unref_object OstreeDeployment *merge_deployment = NULL;
  gs_unref_object OstreeDeployment *new_deployment = NULL;
  gs_free char *origin_description = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_unref_ptrarray GPtrArray *new_deployments =
    g_ptr_array_new_with_free_func (g_object_unref);
  GKeyFile *cur_origin;
  GKeyFile *new_origin;
  gs_free char *tmp_deploy_dir = NULL;
  gs_free char *cur_origin_refspec = NULL;
  gs_free char *cur_origin_baserefspec = NULL;
  gs_strfreev char **cur_origin_packages = NULL;
  gs_unref_hashtable GHashTable *cur_origin_pkgrequests = g_hash_table_new (g_str_hash, g_str_equal);
  gs_unref_hashtable GHashTable *new_pkgrequests = g_hash_table_new (g_str_hash, g_str_equal);
  gs_unref_object HifContext *hifctx = NULL;
  gs_unref_object GFile *merge_deploydir = NULL;
  gs_unref_hashtable GHashTable *layer_new_packages = g_hash_table_new (g_str_hash, g_str_equal);
  guint i;
  const char *osname;
  
  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_NONE, cancellable,
                                       NULL,
                                       error))
    goto out;

  if (argc < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "At least one PACKAGE must be specified");
      goto out;
    }

  sysroot_path = g_file_new_for_path (opt_sysroot);
  sysroot = ostree_sysroot_new (sysroot_path);
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;
  
  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  if (booted_deployment == NULL && opt_osname == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system and no OS specified");
      goto out;
    }

  osname = opt_osname ? opt_osname : ostree_deployment_get_osname (booted_deployment);

  merge_deployment = ostree_sysroot_get_merge_deployment (sysroot, osname);
  if (merge_deployment == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No deployments found for osname %s", opt_osname);
      goto out;
    }

  merge_deploydir = ostree_sysroot_get_deployment_directory (sysroot, merge_deployment);

  cur_origin = ostree_deployment_get_origin (merge_deployment);
  cur_origin_refspec = g_key_file_get_string (cur_origin, "origin", "refspec", NULL);
  cur_origin_baserefspec = g_key_file_get_string (cur_origin, "origin", "baserefspec", NULL);
  if (!(cur_origin_refspec || cur_origin_baserefspec))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin/refspec or origin/baserefspec in current deployment origin");
      goto out;
    }
  if (cur_origin_baserefspec)
    {
      char **strviter;
      cur_origin_packages = g_key_file_get_string_list (cur_origin, "packages", "requested", NULL, NULL);
      for (strviter = cur_origin_packages; strviter && *strviter; strviter++)
        {
          const char *pkg = *strviter;
          g_hash_table_add (cur_origin_pkgrequests, (char*)pkg);
          g_hash_table_add (new_pkgrequests, (char*)pkg);
        }
    }

  for (i = 1; i < argc; i++)
    {
      const char *desired_pkg = argv[i];

      if (g_hash_table_contains (cur_origin_pkgrequests, desired_pkg))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Package '%s' is already requested", desired_pkg);
          goto out;
        }

      g_hash_table_add (new_pkgrequests, (char*)desired_pkg);
    }

  /* Determine whether the package is already installed */
  { g_autoptr(RpmOstreeRefSack) rsack = NULL;

    rsack = rpmostree_get_refsack_for_root (AT_FDCWD, gs_file_get_path_cached (merge_deploydir),
                                            cancellable, error);
    if (!rsack)
      goto out;

    for (i = 1; i < argc; i++)
      {
        const char *desired_pkg = argv[i];
        _cleanup_hyquery_ HyQuery query = NULL;
        _cleanup_hypackagelist_ HyPackageList pkglist = NULL;

        query = hy_query_create (rsack->sack);
        hy_query_filter (query, HY_PKG_NAME, HY_EQ, desired_pkg);
        hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
        pkglist = hy_query_run (query);

        if (hy_packagelist_count (pkglist) == 0)
          g_hash_table_add (layer_new_packages, (char*)desired_pkg);
      }
  }

  hifctx = _rpmostree_libhif_get_default ();
  { gs_free char *reposdir =
      g_build_filename (gs_file_get_path_cached (merge_deploydir), "etc/yum.repos.d", NULL);
    hif_context_set_repo_dir (hifctx, reposdir);
  }

  /* We have a hardcoded tmp-deploy because we have to set up librpm
   * to point at it before creating the transaction.
   *
   * The flow here is to make a *copy* of the current rpmdb so that
   * rpm knows what's installed.
   *
   * We'll rename it back into place before committing.
   */
  { const char *repo_root = gs_file_get_path_cached (ostree_repo_get_path (repo));
    gs_unref_object GFile *src_rpmdb = NULL;
    gs_unref_object GFile *dest_root = NULL;
    gs_unref_object GFile *dest_usrshare = NULL;
    gs_unref_object GFile *dest_rpmdb = NULL;

    tmp_deploy_dir = g_build_filename (repo_root, "tmp/tmp-deploy", NULL);

    if (!gs_shutil_rm_rf_at (AT_FDCWD, tmp_deploy_dir, cancellable, error))
      goto out;

    src_rpmdb = g_file_resolve_relative_path (merge_deploydir, "usr/share/rpm");
    dest_root = g_file_new_for_path (tmp_deploy_dir);
    dest_rpmdb = g_file_resolve_relative_path (dest_root, "usr/share/rpm");
    dest_usrshare = g_file_get_parent (dest_rpmdb);

    if (!gs_file_ensure_directory (dest_usrshare, TRUE, cancellable, error))
      goto out;

    if (!gs_shutil_cp_a (src_rpmdb, dest_rpmdb, cancellable, error))
      goto out;
  }

  hif_context_set_install_root (hifctx, tmp_deploy_dir);
  /* Note this path is relative to the install root */
  hif_context_set_rpm_macro (hifctx, "_dbpath", "/usr/share/rpm");

  if (!_rpmostree_libhif_setup (hifctx, cancellable, error))
    goto out;
      
  if (g_hash_table_size (layer_new_packages) > 0)
    {
      HifTransaction *hiftx;
      HyGoal goal;
      GHashTableIter hashiter;
      gpointer hkey, hvalue;

      /* --- Downloading metadata --- */
      { g_auto(GLnxConsoleRef) console = { 0, };
        gs_unref_object HifState *hifstate = hif_state_new ();
        guint progress_sigid;

        progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                           G_CALLBACK (on_hifstate_percentage_changed), 
                                           "Downloading: ");

        glnx_console_lock (&console);

        if (!hif_context_setup_sack (hifctx, hifstate, error))
          goto out;

        g_signal_handler_disconnect (hifstate, progress_sigid);
      }

      hiftx = hif_context_get_transaction (hifctx);

      g_hash_table_iter_init (&hashiter, layer_new_packages);
      while (g_hash_table_iter_next (&hashiter, &hkey, &hvalue))
        {
          const char *pkg = hkey;

          if (!hif_context_install (hifctx, pkg, error))
            goto out;
        }

      goal = hif_context_get_goal (hifctx);

      if (!hif_transaction_depsolve (hiftx, goal, NULL, error))
        goto out;

      /* --- Downloading packages --- */
      { g_auto(GLnxConsoleRef) console = { 0, };
        gs_unref_object HifState *hifstate = hif_state_new ();
        guint progress_sigid;

        progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                           G_CALLBACK (on_hifstate_percentage_changed), 
                                           "Downloading: ");

        glnx_console_lock (&console);

        if (!hif_transaction_download (hiftx, hifstate, error))
          goto out;

        g_signal_handler_disconnect (hifstate, progress_sigid);
      }

      /* find any packages without valid GPG signatures */
#if 0
      if (!hif_transaction_check_untrusted (hiftx, goal, error))
        goto out;
#endif
    }

  new_origin = _rpmostree_util_keyfile_clone (cur_origin);
  (void) g_key_file_remove_key (new_origin, "origin", "refspec", NULL);
  g_key_file_set_value (new_origin, "origin", "baserefspec", cur_origin_baserefspec ? cur_origin_baserefspec : cur_origin_refspec);

  /* Add previous package requests with newly requested packages */
  { gs_unref_ptrarray GPtrArray *new_requested_pkglist = g_ptr_array_new ();
    GHashTableIter hashiter;
    gpointer hkey, hvalue;
    
    g_hash_table_iter_init (&hashiter, cur_origin_pkgrequests);
    while (g_hash_table_iter_next (&hashiter, &hkey, &hvalue))
      g_ptr_array_add (new_requested_pkglist, hkey);

    g_hash_table_iter_init (&hashiter, layer_new_packages);
    while (g_hash_table_iter_next (&hashiter, &hkey, &hvalue))
      g_ptr_array_add (new_requested_pkglist, hkey);

    g_key_file_set_string_list (new_origin, "packages", "requested",
                                (const char*const*)new_requested_pkglist->pdata,
                                new_requested_pkglist->len);
  }

  if (!ostree_sysroot_deploy_tree (sysroot, osname,
                                   ostree_deployment_get_csum (merge_deployment),
                                   new_origin,
                                   merge_deployment,
                                   NULL,
                                   &new_deployment,
                                   cancellable, error))
    goto out;

  { gs_unref_object GFile *new_deploydir = 
      ostree_sysroot_get_deployment_directory (sysroot, new_deployment);

    /* Now RPM may have initialized the package root...blow it away again. */
    if (!gs_shutil_rm_rf_at (AT_FDCWD, tmp_deploy_dir, cancellable, error))
      goto out;
    
    if (!ostree_sysroot_deployment_set_mutable (sysroot, new_deployment, TRUE,
                                                cancellable, error))
      goto out;

    /* Ok, now move the deploydir into the hardcoded path... */
    if (rename (gs_file_get_path_cached (new_deploydir), tmp_deploy_dir) != 0)
      {
        gs_set_error_from_errno (error, errno);
        goto out;
      }

    if (!overlay_packages_in_deploydir (hifctx, tmp_deploy_dir,
                                        cancellable, error))
      goto out;

    g_clear_object (&hifctx);

    /* Done, move it back */
    if (rename (tmp_deploy_dir, gs_file_get_path_cached (new_deploydir)) != 0)
      {
        gs_set_error_from_errno (error, errno);
        goto out;
      }

    if (!ostree_sysroot_deployment_set_mutable (sysroot, new_deployment, FALSE,
                                                cancellable, error))
      goto out;
  }

  if (!ostree_sysroot_simple_write_deployment (sysroot, osname, new_deployment,
                                               merge_deployment, 0,
                                               cancellable, error))
    goto out;

#ifdef HAVE_PATCHED_HAWKEY_AND_LIBSOLV
  if (!rpmostree_print_treepkg_diff (sysroot, cancellable, error))
    goto out;
#endif

  ret = TRUE;
 out:
  return ret;
}
