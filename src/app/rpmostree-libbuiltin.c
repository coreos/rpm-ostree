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

#include "rpmostree-libbuiltin.h"
#include "rpmostree.h"
#include "rpmostree-cleanup.h"

gboolean
rpmostree_print_treepkg_diff_from_sysroot_path (const gchar *sysroot_path,
                                                GCancellable *cancellable,
                                                GError **error)
{
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gs_unref_object GFile *sysroot_file = NULL;
  gboolean ret = FALSE;

  sysroot_file = g_file_new_for_path (sysroot_path);
  sysroot = ostree_sysroot_new (sysroot_file);

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  ret = rpmostree_print_treepkg_diff (sysroot, cancellable, error);

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

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  g_assert (deployments->len > 1);
  new_deployment = deployments->pdata[0];

  if (booted_deployment && new_deployment != booted_deployment)
    {
      gs_unref_object OstreeRepo *repo = NULL;
      const char *from_rev = ostree_deployment_get_csum (booted_deployment);
      const char *to_rev = ostree_deployment_get_csum (new_deployment);
      g_autoptr(GPtrArray) removed = NULL;
      g_autoptr(GPtrArray) added = NULL;
      g_autoptr(GPtrArray) modified_old = NULL;
      g_autoptr(GPtrArray) modified_new = NULL;
      guint i;

      if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
        goto out;

      if (!rpm_ostree_db_diff (repo, from_rev, to_rev,
                               &removed, &added, &modified_old, &modified_new,
                               cancellable, error))
        goto out;

      if (modified_old->len > 0)
        g_print ("Changed:\n");

      for (i = 0; i < modified_old->len; i++)
        {
          RpmOstreePackage *oldpkg = modified_old->pdata[i];
          RpmOstreePackage *newpkg;
          const char *name = rpm_ostree_package_get_name (oldpkg);

          g_assert_cmpuint (i, <, modified_new->len);

          newpkg = modified_new->pdata[i];

          g_print ("  %s %s -> %s\n", name,
                   rpm_ostree_package_get_evr (oldpkg),
                   rpm_ostree_package_get_evr (newpkg));
        }

      if (removed->len > 0)
        g_print ("Removed:\n");
      for (i = 0; i < removed->len; i++)
        {
          RpmOstreePackage *pkg = removed->pdata[i];
          const char *nevra = rpm_ostree_package_get_nevra (pkg);

          g_print ("  %s\n", nevra);
        }

      if (added->len > 0)
        g_print ("Added:\n");
      for (i = 0; i < added->len; i++)
        {
          RpmOstreePackage *pkg = added->pdata[i];
          const char *nevra = rpm_ostree_package_get_nevra (pkg);

          g_print ("  %s\n", nevra);
        }
    }

  ret = TRUE;
 out:
  return ret;
}
