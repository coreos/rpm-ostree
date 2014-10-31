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

#include "rpmostree-treepkgdiff.h"
#include "rpmostree-hawkey-utils.h"

gboolean
rpmostree_get_pkglist_for_root (GFile            *root,
                                HySack           *out_sack,
                                HyPackageList    *out_pkglist,
                                GCancellable     *cancellable,
                                GError          **error)
{
  gboolean ret = FALSE;
  int rc;
  _cleanup_hysack_ HySack sack = NULL;
  _cleanup_hyquery_ HyQuery query = NULL;
  _cleanup_hypackagelist_ HyPackageList pkglist = NULL;

  sack = hy_sack_create (NULL, NULL, gs_file_get_path_cached (root), 0);
  if (sack == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create sack cache");
      goto out;
    }

  rc = hy_sack_load_system_repo (sack, NULL, 0);
  if (!hif_rc_to_gerror (rc, error))
    {
      g_prefix_error (error, "Failed to load system repo: ");
      goto out;
    }
  query = hy_query_create (sack);
  hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  pkglist = hy_query_run (query);

  ret = TRUE;
  gs_transfer_out_value (out_sack, &sack);
  gs_transfer_out_value (out_pkglist, &pkglist);
 out:
  return ret;
}

static gboolean
print_rpmdb_diff (GFile          *oldroot,
                  GFile          *newroot,
                  GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  _cleanup_hysack_ HySack old_sack = NULL;
  _cleanup_hypackagelist_ HyPackageList old_pkglist = NULL;
  _cleanup_hysack_ HySack new_sack = NULL;
  _cleanup_hypackagelist_ HyPackageList new_pkglist = NULL;
  guint i;
  HyPackage pkg;
  gboolean printed_header = FALSE;

  if (!rpmostree_get_pkglist_for_root (oldroot, &old_sack, &old_pkglist,
                                       cancellable, error))
    goto out;

  if (!rpmostree_get_pkglist_for_root (newroot, &new_sack, &new_pkglist,
                                       cancellable, error))
    goto out;
  
  printed_header = FALSE;
  FOR_PACKAGELIST(pkg, new_pkglist, i)
    {
      _cleanup_hyquery_ HyQuery query = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      
      query = hy_query_create (old_sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, hy_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_EVR, HY_NEQ, hy_package_get_evr (pkg));
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      pkglist = hy_query_run (query);
      if (hy_packagelist_count (pkglist) > 0)
        {
          gs_free char *nevra = hy_package_get_nevra (pkg);
          if (!printed_header)
            {
              g_print ("Changed:\n");
              printed_header = TRUE;
            }
          g_print ("  %s\n", nevra);
        }
    }

  printed_header = FALSE;
  FOR_PACKAGELIST(pkg, old_pkglist, i)
    {
      _cleanup_hyquery_ HyQuery query = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      
      query = hy_query_create (new_sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, hy_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      pkglist = hy_query_run (query);
      if (hy_packagelist_count (pkglist) == 0)
        {
          gs_free char *nevra = hy_package_get_nevra (pkg);
          if (!printed_header)
            {
              g_print ("Removed:\n");
              printed_header = TRUE;
            }
          g_print ("  %s\n", nevra);
        }
    }

  printed_header = FALSE;
  FOR_PACKAGELIST(pkg, new_pkglist, i)
    {
      _cleanup_hyquery_ HyQuery query = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      
      query = hy_query_create (old_sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, hy_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      pkglist = hy_query_run (query);
      if (hy_packagelist_count (pkglist) == 0)
        {
          gs_free char *nevra = hy_package_get_nevra (pkg);
          if (!printed_header)
            {
              g_print ("Added:\n");
              printed_header = TRUE;
            }
          g_print ("  %s\n", nevra);
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_print_treepkg_diff (OstreeSysroot    *sysroot,
                              GCancellable     *cancellable,
                              GError          **error)
{
  gboolean ret = FALSE;
  OstreeDeployment *booted_deployment;
  OstreeDeployment *new_deployment;
  gs_unref_ptrarray GPtrArray *deployments = 
    ostree_sysroot_get_deployments (sysroot);
  gs_unref_object GFile *booted_root = NULL;
  gs_unref_object GFile *new_root = NULL;

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  
  g_assert (deployments->len > 1);
  new_deployment = deployments->pdata[0];
  
  if (booted_deployment && new_deployment != booted_deployment)
    {
      booted_root = ostree_sysroot_get_deployment_directory (sysroot, booted_deployment);
      new_root = ostree_sysroot_get_deployment_directory (sysroot, new_deployment);
      
      if (!print_rpmdb_diff (booted_root, new_root, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

