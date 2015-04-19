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
#include "rpmostree-priv.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-rpm-util.h"

/**
 * SECTION:librpmostree-dbquery
 * @title: Query RPM database
 * @short_description: Access the RPM database in commits
 *
 * These APIs provide queryable access to the RPM database inside an
 * OSTree repository.
 */

static RpmOstreeRefSack *
get_refsack_for_commit (OstreeRepo                *repo,
                        const char                *ref,
                        GCancellable              *cancellable,
                        GError                   **error)
{
  g_autofree char *commit = NULL;
  g_autofree char *tempdir = g_strdup ("/tmp/rpmostree-dbquery-XXXXXXXX");
  OstreeRepoCheckoutOptions checkout_options = { 0, };
  glnx_fd_close int tempdir_dfd = -1;

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

  {
    HySack hsack; 

    if (!rpmostree_get_sack_for_root (tempdir_dfd, ".",
                                      &hsack, cancellable, error))
      goto out;

    return _rpm_ostree_refsack_new (hsack, AT_FDCWD, tempdir);
  }

 out:
  return NULL;
}

static GPtrArray *
query_all_packages_in_sack (RpmOstreeRefSack *rsack)
{
  _cleanup_hyquery_ HyQuery hquery = NULL;
  _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
  GPtrArray *result;
  int i, c;

  hquery = hy_query_create (rsack->sack);
  hy_query_filter (hquery, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  pkglist = hy_query_run (hquery);

  result = g_ptr_array_new_with_free_func (g_object_unref);
  
  c = hy_packagelist_count (pkglist);
  for (i = 0; i < c; i++)
    {
      HyPackage pkg = hy_packagelist_get (pkglist, i);
      g_ptr_array_add (result, _rpm_ostree_package_new (rsack, pkg));
    }
  
  return g_steal_pointer (&result);
}

/**
 * rpm_ostree_db_query:
 * @repo: An OSTree repository
 * @ref: A branch name or commit
 * @query: (allow-none): Currently, this must be %NULL
 * @cancellable: Cancellable
 * @error: Error
 *
 * Query the RPM packages present in the @ref branch or commit in
 * @repo. At present, @query must be %NULL; all packages will be
 * returned.  A future enhancement to this API may allow querying a
 * subset of packages.
 *
 * Returns: (transfer container) (element-type RpmOstreePackage): A query result, or %NULL on error
 */
GPtrArray *
rpm_ostree_db_query (OstreeRepo                *repo,
                     const char                *ref,
                     GVariant                  *query,
                     GCancellable              *cancellable,
                     GError                   **error)
{
  g_autoptr(RpmOstreeRefSack) rsack = NULL;

  g_return_val_if_fail (query == NULL, FALSE);

  rsack = get_refsack_for_commit (repo, ref, cancellable, error);

  return query_all_packages_in_sack (rsack);
}

/**
 * rpm_ostree_db_diff:
 * @repo: An OSTree repository
 * @orig_ref: Original ref (branch or commit)
 * @new_ref: New ref (branch or commit)
 * @query: (allow-none): A query, currently must be %NULL
 * @out_removed: (out) (transfer container) (element-type RpmOstreePackage): Return location for removed packages
 * @out_added: (out) (transfer container) (element-type RpmOstreePackage): Return location for added packages
 * @out_modified_old: (out) (transfer container) (element-type RpmOstreePackage): Return location for modified old packages
 * @out_modified_new: (out) (transfer container) (element-type RpmOstreePackage): Return location for modified new packages
 *
 * Compute the RPM package delta between two commits.  Currently you
 * must use %NULL for the @query parameter; in a future version this
 * function may allow looking at a subset of the packages.
 *
 * The @out_modified_old and @out_modified_new arrays will always be
 * the same length, and indicies will refer to the same base package
 * name.  It is possible in RPM databases to have multiple packages
 * installed with the same name; in this case, the behavior will
 * depend on whether the package set is transitioning from 1 -> N or N
 * -> 1.  In the former case, an arbitrary single instance of one of
 * the new packages will be in @out_modified_new.  If the latter, then
 * multiple entries with the same name will be returned in
 * the array @out_modified_old, with each having a reference to the
 * single corresponding new package.
 */
gboolean
rpm_ostree_db_diff (OstreeRepo               *repo,
                    const char               *orig_ref,
                    const char               *new_ref,
                    GVariant                 *query,
                    GPtrArray               **out_removed,
                    GPtrArray               **out_added,
                    GPtrArray               **out_modified_old,
                    GPtrArray               **out_modified_new,
                    GCancellable             *cancellable,
                    GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(RpmOstreeRefSack) orig_sack = NULL;
  g_autoptr(RpmOstreeRefSack) new_sack = NULL;
  _cleanup_hypackagelist_ HyPackageList orig_pkglist = NULL;
  _cleanup_hypackagelist_ HyPackageList new_pkglist = NULL;
  g_autoptr(GPtrArray) ret_removed = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(GPtrArray) ret_added = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(GPtrArray) ret_modified_old = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(GPtrArray) ret_modified_new = g_ptr_array_new_with_free_func (g_object_unref);
  guint i;
  HyPackage pkg;

  g_return_val_if_fail (query == NULL, FALSE);
  g_return_val_if_fail (out_removed != NULL && out_added != NULL &&
                        out_modified_old != NULL && out_modified_new != NULL, FALSE);

  orig_sack = get_refsack_for_commit (repo, orig_ref, cancellable, error);
  if (!orig_sack)
    goto out;

  { _cleanup_hyquery_ HyQuery query = hy_query_create (orig_sack->sack);
    hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
    orig_pkglist = hy_query_run (query);
  }

  new_sack = get_refsack_for_commit (repo, new_ref, cancellable, error);
  if (!new_sack)
    goto out;

  { _cleanup_hyquery_ HyQuery query = hy_query_create (new_sack->sack);
    hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
    new_pkglist = hy_query_run (query);
  }

  FOR_PACKAGELIST(pkg, new_pkglist, i)
    {
      _cleanup_hyquery_ HyQuery query = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      guint count;
      HyPackage oldpkg;
      
      query = hy_query_create (orig_sack->sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, hy_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_EVR, HY_NEQ, hy_package_get_evr (pkg));
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      pkglist = hy_query_run (query);

      count = hy_packagelist_count (pkglist);
      if (count > 0)
        {
          /* See comment above about transitions from N -> 1 */
          oldpkg = hy_packagelist_get (pkglist, 0);
          
          g_ptr_array_add (ret_modified_old, _rpm_ostree_package_new (orig_sack, oldpkg));
          g_ptr_array_add (ret_modified_new, _rpm_ostree_package_new (new_sack, pkg));
        }
    }

  FOR_PACKAGELIST(pkg, orig_pkglist, i)
    {
      _cleanup_hyquery_ HyQuery query = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      
      query = hy_query_create (new_sack->sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, hy_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      pkglist = hy_query_run (query);

      if (hy_packagelist_count (pkglist) == 0)
        g_ptr_array_add (ret_removed, _rpm_ostree_package_new (orig_sack, pkg));
    }

  FOR_PACKAGELIST(pkg, new_pkglist, i)
    {
      _cleanup_hyquery_ HyQuery query = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      
      query = hy_query_create (orig_sack->sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, hy_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      pkglist = hy_query_run (query);

      if (hy_packagelist_count (pkglist) == 0)
        g_ptr_array_add (ret_added, _rpm_ostree_package_new (new_sack, pkg));
    }

  ret = TRUE;
  *out_removed = g_steal_pointer (&ret_removed);
  *out_added = g_steal_pointer (&ret_added);
  *out_modified_old = g_steal_pointer (&ret_modified_old);
  *out_modified_new = g_steal_pointer (&ret_modified_new);
 out:
  return ret;
}
