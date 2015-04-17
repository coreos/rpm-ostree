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
#include "rpmostree-treepkgdiff.h"

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
