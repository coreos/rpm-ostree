/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include "ostree.h"

#include <libglnx.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmdb.h>
#include <libhif.h>
#include <libhif/hif-utils.h>
#include <libgsystem.h>

#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-sysroot.h"
#include "rpmostree-sysroot-upgrader.h"
#include "rpmostreed-utils.h"
#include "rpmostree-hif.h"
#include "rpmostree-rpm-util.h"

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  char **packages;
  gboolean reboot;
} PkgAddTransaction;

typedef RpmostreedTransactionClass PkgAddTransactionClass;

GType pkg_add_transaction_get_type (void);

G_DEFINE_TYPE (PkgAddTransaction,
               pkg_add_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
pkg_add_transaction_finalize (GObject *object)
{
  PkgAddTransaction *self;

  self = (PkgAddTransaction *) object;
  g_free (self->osname);
  g_strfreev (self->packages);

  G_OBJECT_CLASS (pkg_add_transaction_parent_class)->finalize (object);
}

static gboolean
copy_dir_contents_nonrecurse_at (int         src_dfd,
				 const char *srcpath,
				 int         dest_dfd,
				 GCancellable  *cancellable,
				 GError      **error)
{
  gboolean ret = FALSE;
  g_auto(GLnxDirFdIterator) dfd_iter = { FALSE, };
  struct dirent *dent = NULL;

  if (!glnx_dirfd_iterator_init_at (src_dfd, srcpath, TRUE,
				    &dfd_iter, error))
    goto out;
  
  while (glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
    {
      if (dent == NULL)
	break;
      if (!glnx_file_copy_at (dfd_iter.fd, dent->d_name, NULL, dest_dfd, dent->d_name, 0,
			      cancellable, error))
	{
	  goto out;
	}
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
instroot_make_rpmdb_copy (int             dfd,
			  const char     *root,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  const char const rpmdb_path[] = "usr/share/rpm";
  const char const rpmdb_path_tmp[] = "usr/share/rpm.tmp";
  glnx_fd_close int root_dfd = -1;
  glnx_fd_close int target_rpmdb_dfd = -1;

  if (!glnx_opendirat (dfd, root, TRUE, &root_dfd, error))
    goto out;

  if (TEMP_FAILURE_RETRY (renameat (root_dfd, rpmdb_path, root_dfd, rpmdb_path_tmp)) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (TEMP_FAILURE_RETRY (mkdirat (root_dfd, rpmdb_path, 0755)) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (!glnx_opendirat (root_dfd, rpmdb_path, TRUE, &target_rpmdb_dfd, error))
    goto out;

  if (!copy_dir_contents_nonrecurse_at (root_dfd, rpmdb_path_tmp, target_rpmdb_dfd, cancellable, error))
    goto out;

  if (!glnx_shutil_rm_rf_at (root_dfd, rpmdb_path_tmp, cancellable, error))
    goto out;

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
			       int              dfd,
                               const char      *deploydir,
                               GCancellable    *cancellable,
                               GError         **error)
{
  gboolean ret = FALSE;

  if (!instroot_make_rpmdb_copy (dfd, deploydir, cancellable, error))
    {
      g_prefix_error (error, "While copying target rpmdb: ");
      goto out;
    }
  
  /* --- Run transaction --- */
  { g_auto(GLnxConsoleRef) console = { 0, };
    glnx_unref_object HifState *hifstate = hif_state_new ();
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

static gboolean
pkg_add_transaction_execute (RpmostreedTransaction *transaction,
			     GCancellable *cancellable,
			     GError **error)
{
  gboolean ret = FALSE;
  const char const tmp_deploy[] = "repo/tmp/tmp-deploy";
  PkgAddTransaction *self;
  OstreeSysroot *sysroot;
  int sysroot_fd; /* Borrowed */

  glnx_unref_object RpmOstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  glnx_unref_object OstreeDeployment *merge_deployment = NULL;
  glnx_unref_object OstreeDeployment *new_deployment = NULL;
  g_autofree char *merge_deployment_dirpath = NULL;
  glnx_fd_close int merge_deployment_dirfd = -1;
  glnx_fd_close int tmp_deploy_rpmdb_fd = -1;
  g_autoptr(GKeyFile) origin = NULL;
  gs_unref_object HifContext *hifctx = NULL;
  g_autoptr(GHashTable) cur_origin_pkgrequests = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr(GHashTable) new_pkgrequests = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr(GHashTable) layer_new_packages = g_hash_table_new (g_str_hash, g_str_equal);

  self = (PkgAddTransaction *) transaction;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);
  sysroot_fd = ostree_sysroot_get_fd (sysroot);

  merge_deployment = ostree_sysroot_get_merge_deployment (sysroot, self->osname);
  if (merge_deployment == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No deployments found for osname %s", self->osname);
      goto out;
    }

  merge_deployment_dirpath = ostree_sysroot_get_deployment_dirpath (sysroot, merge_deployment);

  if (!glnx_opendirat (sysroot_fd, merge_deployment_dirpath, TRUE,
		       &merge_deployment_dirfd, error))
    goto out;

  upgrader = rpmostree_sysroot_upgrader_new (sysroot, self->osname, 0,
					     cancellable, error);
  if (upgrader == NULL)
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  origin = rpmostree_sysroot_upgrader_dup_origin (upgrader);
  if (origin == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Booted deployment has no origin");
      goto out;
    }

  {
    const char *cur_origin_refspec =
      rpmostree_sysroot_upgrader_get_refspec (upgrader);
    const char *const *cur_origin_packages =
      rpmostree_sysroot_upgrader_get_packages (upgrader);
    const char *const*strviter;

    g_assert (cur_origin_refspec);
    
    for (strviter = cur_origin_packages; strviter && *strviter; strviter++)
      {
	const char *pkg = *strviter;
	g_hash_table_add (cur_origin_pkgrequests, (char*)pkg);
	g_hash_table_add (new_pkgrequests, (char*)pkg);
      }

    (void) g_key_file_remove_key (origin, "origin", "refspec", NULL);
    g_key_file_set_value (origin, "origin", "baserefspec", cur_origin_refspec);
  }

  {
    char **iter = self->packages;
    g_autoptr(RpmOstreeRefSack) rsack = NULL;

    rsack = rpmostree_get_refsack_for_root (sysroot_fd,
					    merge_deployment_dirpath,
                                            cancellable, error);
    if (!rsack)
      goto out;

    for (; iter && *iter; iter++)
      {
	const char *desired_pkg = *iter;
	_cleanup_hyquery_ HyQuery query = NULL;
	_cleanup_hypackagelist_ HyPackageList pkglist = NULL;
	
	if (g_hash_table_contains (cur_origin_pkgrequests, desired_pkg))
	  {
	    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			 "Package '%s' is already requested", desired_pkg);
	    goto out;
	  }

	/* It's now requested */
	g_hash_table_add (new_pkgrequests, (char*)desired_pkg);

        query = hy_query_create (rsack->sack);
        hy_query_filter (query, HY_PKG_NAME, HY_EQ, desired_pkg);
        hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
        pkglist = hy_query_run (query);

	/* This one tracks whether it actually needs to be installed */
        if (hy_packagelist_count (pkglist) == 0)
          g_hash_table_add (layer_new_packages, (char*)desired_pkg);
      }
  }

  hifctx = _rpmostree_libhif_get_default ();
  {
    g_autofree char *reposdir =
      g_build_filename (merge_deployment_dirpath, "etc/yum.repos.d", NULL);
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
  { 
    g_autofree char *tmp_deploy_rpmdb = g_strconcat (tmp_deploy, "/usr/share/rpm", NULL);

    if (!glnx_shutil_rm_rf_at (sysroot_fd, tmp_deploy, cancellable, error))
      goto out;

    if (!glnx_shutil_mkdir_p_at (sysroot_fd, tmp_deploy, 0755,
				 cancellable, error))
      goto out;

    { 
      if (!glnx_shutil_mkdir_p_at (sysroot_fd, tmp_deploy_rpmdb, 0755,
				   cancellable, error))
	goto out;
    }

    { g_autofree char *merge_deploy_rpmdb = g_strconcat (merge_deployment_dirpath, "/usr/share/rpm", NULL);
      glnx_fd_close int tmp_deploy_rpmdb_fd = -1;

      if (!glnx_opendirat (sysroot_fd, tmp_deploy_rpmdb, TRUE,
			   &tmp_deploy_rpmdb_fd, error))
	goto out;

      if (!copy_dir_contents_nonrecurse_at (sysroot_fd, merge_deploy_rpmdb, tmp_deploy_rpmdb_fd,
					    cancellable, error))
	{
	  g_prefix_error (error, "While copying current rpmdb: ");
	  goto out;
	}
    }

    { g_autofree char *tmp_deploy_dirpath =
	glnx_fdrel_abspath (sysroot_fd, tmp_deploy);
      hif_context_set_install_root (hifctx, tmp_deploy_dirpath);
    }
  }

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
        glnx_unref_object HifState *hifstate = hif_state_new ();
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
        glnx_unref_object HifState *hifstate = hif_state_new ();
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

    g_key_file_set_string_list (origin, "packages", "requested",
                                (const char*const*)new_requested_pkglist->pdata,
                                new_requested_pkglist->len);
  }

  if (!ostree_sysroot_deploy_tree (sysroot, self->osname,
                                   ostree_deployment_get_csum (merge_deployment),
                                   origin,
                                   merge_deployment,
                                   NULL,
                                   &new_deployment,
                                   cancellable, error))
    goto out;

  { g_autofree char *new_deploydir = 
      ostree_sysroot_get_deployment_dirpath (sysroot, new_deployment);

    /* Now RPM may have initialized the package root...blow it away again. */
    if (!glnx_shutil_rm_rf_at (sysroot_fd, tmp_deploy, cancellable, error))
      goto out;
    
    if (!ostree_sysroot_deployment_set_mutable (sysroot, new_deployment, TRUE,
                                                cancellable, error))
      goto out;

    /* Ok, now move the deploydir into the hardcoded path... */
    if (renameat (sysroot_fd,
		  new_deploydir,
		  sysroot_fd,
		  tmp_deploy) != 0)
      {
        glnx_set_error_from_errno (error);
        goto out;
      }

    if (!overlay_packages_in_deploydir (hifctx, sysroot_fd, tmp_deploy,
                                        cancellable, error))
      goto out;

    g_clear_object (&hifctx);

    /* Done, move it back */
    if (TEMP_FAILURE_RETRY (renameat (sysroot_fd, tmp_deploy,
				      sysroot_fd, new_deploydir)) != 0)
      {
        glnx_set_error_from_errno (error);
        goto out;
      }

    if (!ostree_sysroot_deployment_set_mutable (sysroot, new_deployment, FALSE,
                                                cancellable, error))
      goto out;
  }

  if (!ostree_sysroot_simple_write_deployment (sysroot, self->osname, new_deployment,
                                               merge_deployment, 0,
                                               cancellable, error))
    goto out;

  if (self->reboot)
    rpmostreed_reboot (cancellable, error);

  ret = TRUE;

out:
  return ret;
}

static void
pkg_add_transaction_class_init (PkgAddTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = pkg_add_transaction_finalize;

  class->execute = pkg_add_transaction_execute;
}

static void
pkg_add_transaction_init (PkgAddTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_pkg_add (GDBusMethodInvocation *invocation,
				    OstreeSysroot         *sysroot,
				    const char            *osname,
				    const char * const    *packages,
				    gboolean               reboot,
				    GCancellable          *cancellable,
				    GError               **error)
{
  PkgAddTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);
  g_return_val_if_fail (packages != NULL, NULL);

  self = g_initable_new (pkg_add_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot", sysroot,
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->packages = g_strdupv ((char**)packages);
      self->reboot = reboot;
    }

  return (RpmostreedTransaction *) self;
}
