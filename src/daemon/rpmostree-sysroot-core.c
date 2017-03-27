/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <libglnx.h>
#include <systemd/sd-journal.h>
#include "rpmostreed-utils.h"
#include "rpmostree-util.h"

#include "rpmostree-sysroot-upgrader.h"
#include "rpmostree-core.h"
#include "rpmostree-origin.h"
#include "rpmostree-kernel.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-output.h"

#include "ostree-repo.h"

/**
 * SECTION:rpmostree-sysroot-core
 *
 * This file contains a set of "core logic" functions for operating on
 * a sysroot, right now used by SysrootUpgrader as well as other functions
 * such as cleanup.
 */

/* For each deployment, if they are layered deployments, then create a ref
 * pointing to their bases. This is mostly to work around ostree's auto-ref
 * cleanup. Otherwise we might get into a situation where after the origin ref
 * is updated, we lose our parent, which means that users can no longer
 * add/delete packages on that deployment. (They can always just re-pull it, but
 * let's try to be nice).
 **/
static gboolean
generate_baselayer_refs (OstreeSysroot            *sysroot,
                         OstreeRepo               *repo,
                         GCancellable             *cancellable,
                         GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) refs = NULL;
  g_autoptr(GHashTable) bases =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (!ostree_repo_list_refs_ext (repo, "rpmostree/base", &refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE,
                                  cancellable, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  /* delete all the refs */
  {
    GHashTableIter it;
    gpointer key;

    g_hash_table_iter_init (&it, refs);
    while (g_hash_table_iter_next (&it, &key, NULL))
      {
        const char *ref = key;
        ostree_repo_transaction_set_refspec (repo, ref, NULL);
      }
  }

  /* collect the csums */
  {
    guint i = 0;
    g_autoptr(GPtrArray) deployments =
      ostree_sysroot_get_deployments (sysroot);

    /* existing deployments */
    for (; i < deployments->len; i++)
      {
        OstreeDeployment *deployment = deployments->pdata[i];
        g_autofree char *base_rev = NULL;

        if (!rpmostree_deployment_get_layered_info (repo, deployment, NULL,
                                                    &base_rev, NULL, error))
          goto out;

        if (base_rev)
          g_hash_table_add (bases, g_steal_pointer (&base_rev));
      }
  }

  /* create the new refs */
  {
    guint i = 0;
    GHashTableIter it;
    gpointer key;

    g_hash_table_iter_init (&it, bases);
    while (g_hash_table_iter_next (&it, &key, NULL))
      {
        const char *base = key;
        g_autofree char *ref = g_strdup_printf ("rpmostree/base/%u", i++);
        ostree_repo_transaction_set_refspec (repo, ref, base);
      }
  }

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}

/* For all packages in the sack, generate a cached refspec and add it
 * to @referenced_pkgs. This is necessary to implement garbage
 * collection of layered package refs.
 */
static gboolean
add_package_refs_to_set (RpmOstreeRefSack *rsack,
                         GHashTable *referenced_pkgs,
                         GCancellable *cancellable,
                         GError **error)
{
  g_autoptr(GPtrArray) pkglist = NULL;
  hy_autoquery HyQuery query = hy_query_create (rsack->sack);
  hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  pkglist = hy_query_run (query);

  /* TODO: convert this to an iterator to avoid lots of malloc */

  if (pkglist->len == 0)
    sd_journal_print (LOG_WARNING, "Failed to find any packages in root");
  else
    {
      for (guint i = 0; i < pkglist->len; i++)
        {
          DnfPackage *pkg = pkglist->pdata[i];
          g_autofree char *pkgref = rpmostree_get_cache_branch_pkg (pkg);
          g_hash_table_add (referenced_pkgs, g_steal_pointer (&pkgref));
        }
    }

  return TRUE;
}

/* Loop over all deployments, gathering all referenced NEVRAs for
 * layered packages.  Then delete any cached pkg refs that aren't in
 * that set.
 */
static gboolean
clean_pkgcache_orphans (OstreeSysroot            *sysroot,
                        OstreeRepo               *repo,
                        GCancellable             *cancellable,
                        GError                  **error)
{
  glnx_unref_object OstreeRepo *pkgcache_repo = NULL;
  g_autoptr(GPtrArray) deployments =
    ostree_sysroot_get_deployments (sysroot);
  g_autoptr(GHashTable) current_refs = NULL;
  g_autoptr(GHashTable) referenced_pkgs = /* cache refs of packages we want to keep */
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GHashTableIter hiter;
  gpointer hkey, hvalue;
  gint n_objects_total;
  gint n_objects_pruned;
  guint64 freed_space;
  guint n_freed = 0;

  if (!rpmostree_get_pkgcache_repo (repo, &pkgcache_repo, cancellable, error))
    return FALSE;

  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      gboolean is_layered;

      if (!rpmostree_deployment_get_layered_info (repo, deployment, &is_layered,
                                                  NULL, NULL, error))
        return FALSE;

      if (is_layered)
        {
          g_autoptr(RpmOstreeRefSack) rsack = NULL;
          g_autofree char *deployment_dirpath = NULL;

          deployment_dirpath = ostree_sysroot_get_deployment_dirpath (sysroot, deployment);

          /* We could do this via the commit object, but it's faster
           * to reuse the existing rpmdb checkout.
           */
          rsack = rpmostree_get_refsack_for_root (ostree_sysroot_get_fd (sysroot),
                                                  deployment_dirpath,
                                                  cancellable, error);
          if (rsack == NULL)
            return FALSE;

          if (!add_package_refs_to_set (rsack, referenced_pkgs,
                                        cancellable, error))
            return FALSE;
        }
    }

  if (!ostree_repo_list_refs_ext (pkgcache_repo, "rpmostree/pkg", &current_refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE,
                                  cancellable, error))
    return FALSE;

  g_hash_table_iter_init (&hiter, current_refs);
  while (g_hash_table_iter_next (&hiter, &hkey, &hvalue))
    {
      const char *ref = hkey;
      if (g_hash_table_contains (referenced_pkgs, ref))
        continue;

      if (!ostree_repo_set_ref_immediate (pkgcache_repo, NULL, ref, NULL,
                                          cancellable, error))
        return FALSE;
      n_freed++;
    }

  if (!ostree_repo_prune (pkgcache_repo, OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY, 0,
                          &n_objects_total, &n_objects_pruned, &freed_space,
                          cancellable, error))
    return FALSE;

  if (n_freed > 0 || freed_space > 0)
    {
      char *freed_space_str = g_format_size_full (freed_space, 0);
      g_print ("Freed pkgcache branches: %u size: %s\n", n_freed, freed_space_str);
    }

  return TRUE;
}

/* Clean up to match the current deployments. This used to be a private static,
 * but is now used by the cleanup txn.
 */
gboolean
rpmostree_syscore_cleanup (OstreeSysroot            *sysroot,
                           OstreeRepo               *repo,
                           GCancellable             *cancellable,
                           GError                  **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo); /* borrowed */

  /* regenerate the baselayer refs in case we just kicked out an ancient layered
   * deployment whose base layer is not needed anymore */
  if (!generate_baselayer_refs (sysroot, repo, cancellable, error))
    return FALSE;

  /* Delete our temporary ref */
  if (!ostree_repo_set_ref_immediate (repo, NULL, RPMOSTREE_TMP_BASE_REF,
                                      NULL, cancellable, error))
    return FALSE;

  /* and shake it loose */
  if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
    return FALSE;

  if (!clean_pkgcache_orphans (sysroot, repo, cancellable, error))
    return FALSE;

  /* delete our checkout dir in case a previous run didn't finish
     successfully */
  if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR,
                             cancellable, error))
    return FALSE;

  return TRUE;
}
