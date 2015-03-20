/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "string.h"

#include "rpmostree-db.h"
#include "rpmostree-cleanup.h"

struct RpmOstreeDbQueryResult 
{
  volatile gint refcount;
  GPtrArray *packages;
};

RpmOstreeDbQueryResult *
rpm_ostree_db_query_ref (RpmOstreeDbQueryResult *result)
{
  g_atomic_int_inc (&result->refcount);
  return result;
}

void
rpm_ostree_db_query_unref (RpmOstreeDbQueryResult *result)
{
  if (!g_atomic_int_dec_and_test (&result->refcount))
    return;

  g_ptr_array_unref (result->packages);
  g_free (result);
}

G_DEFINE_BOXED_TYPE(RpmOstreeDbQueryResult, rpm_ostree_db_query_result,
                    rpm_ostree_db_query_ref,
                    rpm_ostree_db_query_unref);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeDbQueryResult, rpm_ostree_db_query_unref)

/**
 * rpm_ostree_db_query_result_get_packages:
 * @queryresult: Query result
 *
 * Returns: (transfer none) (array zero-terminated=1) (element-type utf8): List of packages, %NULL terminated
 */
const char *const *
rpm_ostree_db_query_result_get_packages (RpmOstreeDbQueryResult *queryresult)
{
  return (const char * const *)queryresult->packages->pdata;
}

/**
 * rpm_ostree_db_query:
 * @repo: An OSTree repository
 * @ref: A branch name or commit
 * @query: (allow-none): Currently, this must be %NULL
 * @out_result: (out) (transfer full): Query reslut
 * @cancellable: Cancellable
 * @error: Error
 *
 * Query the RPM packages present in the @ref branch or commit in
 * @repo. At present, @query must be %NULL; all packages will be
 * returned.  A future enhancement to this API may allow querying a
 * subset of packages.
 */
gboolean
rpm_ostree_db_query (OstreeRepo                *repo,
                     const char                *ref,
                     GVariant                  *query,
                     RpmOstreeDbQueryResult   **out_result,
                     GCancellable              *cancellable,
                     GError                   **error)
{
  gboolean ret = FALSE;
  int rc;
  OstreeRepoCheckoutOptions checkout_options = { 0, };
  g_autofree char *commit = NULL;
  _cleanup_hysack_ HySack sack = NULL;
  _cleanup_hyquery_ HyQuery hquery = NULL;
  _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
  g_autofree char *tempdir = g_strdup ("/tmp/rpmostree-dbquery-XXXXXXXX");
  g_autofree char *rpmdb_tempdir = NULL;
  gs_unref_object GFile* commit_rpmdb = NULL;
  glnx_fd_close int tempdir_dfd = -1;

  g_return_val_if_fail (query == NULL, FALSE);

  if (!ostree_repo_resolve_rev (repo, ref, FALSE, &commit, error))
    goto out;

  if (mkdtemp (tempdir) == NULL)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (!glnx_opendirat (AT_FDCWD, tempdir, FALSE, &tempdir_dfd, error))
    goto out;

  /* Create intermediate dirs */ 
  if (!glnx_shutil_mkdir_p_at (tempdir_dfd, "usr/share", 0777, cancellable, error))
    goto out;

  checkout_options.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
  checkout_options.subpath = "usr/share/rpm";

  if (!ostree_repo_checkout_tree_at (repo, &checkout_options,
                                     tempdir_dfd, "usr/share/rpm",
                                     commit, 
                                     cancellable, error))
    goto out;

#if BUILDOPT_HAWKEY_SACK_CREATE2
  sack = hy_sack_create (NULL, NULL,
                         rpmdb_tempdir,
                         NULL,
                         0);
#else
  sack = hy_sack_create (NULL, NULL,
                         rpmdb_tempdir,
                         0);
#endif
  if (sack == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create sack cache");
      goto out;
    }

  rc = hy_sack_load_system_repo (sack, NULL, 0);
  if (!hif_error_set_from_hawkey (rc, error))
    {
      g_prefix_error (error, "Failed to load system repo: ");
      goto out;
    }
  hquery = hy_query_create (sack);
  hy_query_filter (hquery, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  pkglist = hy_query_run (hquery);

  (void) glnx_shutil_rm_rf_at (AT_FDCWD, tempdir, cancellable, NULL);

  ret = TRUE;
  /* Do output creation now, no errors can be thrown */
  {
    RpmOstreeDbQueryResult *result = g_new0 (RpmOstreeDbQueryResult, 1);
    int i, c;

    result->refcount = 1;
    result->packages = g_ptr_array_new_with_free_func (free);
    
    c = hy_packagelist_count (pkglist);
    for (i = 0; i < c; i++)
      {
        HyPackage pkg = hy_packagelist_get (pkglist, i);
        g_ptr_array_add (result->packages, hy_package_get_nevra (pkg));
      }
    g_ptr_array_add (result->packages, NULL);
    
    *out_result = g_steal_pointer (&result);
  }
 out:
  return ret;
}
