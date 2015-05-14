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

static GVariant *
build_diff_variant (const gchar *name,
                    guint type,
                    RpmOstreePackage *old_package,
                    RpmOstreePackage *new_package)
{
  GVariantBuilder options_builder;
  GVariantBuilder builder;
  g_variant_builder_init (&options_builder, G_VARIANT_TYPE ("a{sv}"));

  if (old_package)
    {
        g_variant_builder_add(&options_builder, "{sv}", "PreviousPackage",
                              rpm_ostree_package_to_variant (old_package));
    }

  if (new_package)
    {
      g_variant_builder_add(&options_builder, "{sv}", "NewPackage",
                            rpm_ostree_package_to_variant (new_package));
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
  g_variant_builder_add(&builder, "s", name);
  g_variant_builder_add(&builder, "u", type);
  g_variant_builder_add_value(&builder, g_variant_builder_end (&options_builder));
  return g_variant_builder_end (&builder);
}

/**
 * rpm_ostree_db_query_all:
 * @repo: An OSTree repository
 * @ref: A branch name or commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Return all of the RPM packages present in the @ref branch or commit
 * in @repo.
 *
 * Returns: (transfer container) (element-type RpmOstreePackage): A query result, or %NULL on error
 */
GPtrArray *
rpm_ostree_db_query_all (OstreeRepo                *repo,
                         const char                *ref,
                         GCancellable              *cancellable,
                         GError                   **error)
{
  g_autoptr(RpmOstreeRefSack) rsack = NULL;

  rsack = get_refsack_for_commit (repo, ref, cancellable, error);

  return query_all_packages_in_sack (rsack);
}

/**
 * rpm_ostree_db_diff:
 * @repo: An OSTree repository
 * @orig_ref: Original ref (branch or commit)
 * @new_ref: New ref (branch or commit)
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

int
rpm_ostree_db_diff_variant_compare_by_name (const void *v1,
                                            const void *v2)

{
  GVariant **v1pp = (GVariant**)v1;
  GVariant *variant1 = *v1pp;
  GVariant **v2pp = (GVariant**)v2;
  GVariant *variant2 = *v2pp;

  gs_free gchar *name1 = NULL;
  gs_free gchar *name2 = NULL;
  g_variant_get_child (variant1, 0, "s", &name1);
  g_variant_get_child (variant2, 0, "s", &name2);

  return g_strcmp0(name1, name2);
}

int
rpm_ostree_db_diff_variant_compare_by_type (const void *v1,
                                            const void *v2)

{
  GVariant **v1pp = (GVariant**)v1;
  GVariant *variant1 = *v1pp;
  GVariant **v2pp = (GVariant**)v2;
  GVariant *variant2 = *v2pp;

  guint type1;
  guint type2;

  g_variant_get_child (variant1, 1, "u", &type1);
  g_variant_get_child (variant2, 1, "u", &type2);

  if (type1 == type2)
    return rpm_ostree_db_diff_variant_compare_by_name (v1, v2);

  return type1 - type2;
}

/**
 * rpm_ostree_db_build_diff_variant
 * @repo: A OstreeRepo
 * @old_ref: old ref to use
 * @new_ref: New ref to use
 * GCancellable: *cancellable
 * GError: **error
 *
 * Returns: A GVariant that represents the differences
 * between the rpm databases on the given refs.
 */
GVariant *
rpm_ostree_db_diff_variant (OstreeRepo *repo,
                            const char *from_rev,
                            const char *to_rev,
                            GCancellable *cancellable,
                            GError **error)
{
  GVariant *variant = NULL;
  GVariantBuilder builder;

  g_autoptr(GPtrArray) removed = NULL;
  g_autoptr(GPtrArray) added = NULL;
  g_autoptr(GPtrArray) modified_old = NULL;
  g_autoptr(GPtrArray) modified_new = NULL;
  g_autoptr(GPtrArray) found = NULL;

  guint i;

  found = g_ptr_array_new ();

  if (!rpm_ostree_db_diff (repo, from_rev, to_rev,
                           &removed, &added, &modified_old,
                           &modified_new, cancellable, error))
    goto out;

  if (modified_old->len > 0)
    {
      for (i = 0; i < modified_old->len; i++)
      {
        guint type = RPM_OSTREE_PACKAGE_UPGRADED;
        RpmOstreePackage *oldpkg = modified_old->pdata[i];
        RpmOstreePackage *newpkg;

        const char *name = rpm_ostree_package_get_name (oldpkg);
        g_assert_cmpuint (i, <, modified_new->len);
        newpkg = modified_new->pdata[i];

        if (rpm_ostree_package_cmp (oldpkg, newpkg) < 0)
              type = RPM_OSTREE_PACKAGE_DOWNGRADED;

        g_ptr_array_add (found,
                         build_diff_variant (name, type, oldpkg, newpkg));
      }
    }

  if (removed->len > 0)
    {
      for (i = 0; i < removed->len; i++)
      {
        RpmOstreePackage *pkg = removed->pdata[i];
        const char *name = rpm_ostree_package_get_name (pkg);
        g_ptr_array_add (found,
                         build_diff_variant (name,
                                             RPM_OSTREE_PACKAGE_REMOVED,
                                             pkg,
                                             NULL));
      }
    }

  if (added->len > 0)
    {
      for (i = 0; i < added->len; i++)
        {
          RpmOstreePackage *pkg = added->pdata[i];
          const char *name = rpm_ostree_package_get_name (pkg);
          g_ptr_array_add (found,
                           build_diff_variant (name,
                                               RPM_OSTREE_PACKAGE_ADDED,
                                               NULL,
                                               pkg));
        }
    }

  g_ptr_array_sort (found, rpm_ostree_db_diff_variant_compare_by_type);
  g_variant_builder_init (&builder, G_VARIANT_TYPE_ARRAY);
  for (i = 0; i < found->len; i++)
    {
      GVariant *v = found->pdata[i];
      g_variant_builder_add_value (&builder, v);
    }

  variant = g_variant_builder_end (&builder);

out:
  return variant;
}
