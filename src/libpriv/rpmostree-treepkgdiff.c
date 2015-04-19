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
#include "rpmostree-cleanup.h"

gboolean
rpmostree_get_sack_for_root (int               dfd,
                             const char       *path,
                             HySack           *out_sack,
                             GCancellable     *cancellable,
                             GError          **error)
{
  gboolean ret = FALSE;
  int rc;
  _cleanup_hysack_ HySack sack = NULL;
  g_autofree char *fullpath = glnx_fdrel_abspath (dfd, path);

  g_return_val_if_fail (out_sack != NULL, FALSE);

#if BUILDOPT_HAWKEY_SACK_CREATE2
  sack = hy_sack_create (NULL, NULL,
                         fullpath,
                         NULL,
                         HY_MAKE_CACHE_DIR);
#else
  sack = hy_sack_create (NULL, NULL,
                         fullpath,
                         HY_MAKE_CACHE_DIR);
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

  ret = TRUE;
  *out_sack = g_steal_pointer (&sack);
 out:
  return ret;
}

gboolean
rpmostree_get_pkglist_for_root (int               dfd,
                                const char       *path,
                                HySack           *out_sack,
                                HyPackageList    *out_pkglist,
                                GCancellable     *cancellable,
                                GError          **error)
{
  gboolean ret = FALSE;
  _cleanup_hysack_ HySack sack = NULL;
  _cleanup_hyquery_ HyQuery query = NULL;
  _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
  g_autofree char *fullpath = glnx_fdrel_abspath (dfd, path);

  if (!rpmostree_get_sack_for_root (dfd, path, &sack, cancellable, error))
    goto out;

  query = hy_query_create (sack);
  hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  pkglist = hy_query_run (query);

  ret = TRUE;
  gs_transfer_out_value (out_sack, &sack);
  gs_transfer_out_value (out_pkglist, &pkglist);
 out:
  return ret;
}

static gint
pkg_array_compare (HyPackage *p_pkg1,
                   HyPackage *p_pkg2)
{
  return hy_package_cmp (*p_pkg1, *p_pkg2);
}

void
rpmostree_print_transaction (HifContext   *hifctx)
{
  guint i;
  g_autoptr(GPtrArray) install;

  install = hif_goal_get_packages (hif_context_get_goal (hifctx),
                                   HIF_PACKAGE_INFO_INSTALL,
                                   HIF_PACKAGE_INFO_REINSTALL,
                                   HIF_PACKAGE_INFO_DOWNGRADE,
                                   HIF_PACKAGE_INFO_UPDATE,
                                   -1);

  g_print ("Transaction: %u packages\n", install->len);
  
  if (install->len == 0)
    g_print ("  (empty)\n");
  else
    {
      g_ptr_array_sort (install, (GCompareFunc) pkg_array_compare);

      for (i = 0; i < install->len; i++)
        {
          HyPackage pkg = install->pdata[i];
          g_print ("  %s\n", hif_package_get_nevra (pkg));
        }
    }
}
