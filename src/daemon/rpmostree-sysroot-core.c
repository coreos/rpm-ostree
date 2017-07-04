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
#include "rpmostree-sysroot-core.h"
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
  GLNX_HASH_TABLE_FOREACH (refs, const char*, ref)
    ostree_repo_transaction_set_refspec (repo, ref, NULL);

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
                                                    &base_rev, NULL, NULL, error))
          goto out;

        if (base_rev)
          g_hash_table_add (bases, g_steal_pointer (&base_rev));
      }
  }

  /* create the new refs */
  {
    guint i = 0;
    GLNX_HASH_TABLE_FOREACH (bases, const char*, base)
      {
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
  g_autoptr(GHashTable) referenced_pkgs = /* cache refs of packages we want to keep */
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_autoptr(OstreeRepo) pkgcache_repo = NULL;
  if (!rpmostree_get_pkgcache_repo (repo, &pkgcache_repo, cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];

      gboolean is_layered;
      if (!rpmostree_deployment_get_layered_info (repo, deployment, &is_layered,
                                                  NULL, NULL, NULL, error))
        return FALSE;

      if (is_layered)
        {
          g_autofree char *deployment_dirpath =
            ostree_sysroot_get_deployment_dirpath (sysroot, deployment);

          /* We could do this via the commit object, but it's faster
           * to reuse the existing rpmdb checkout.
           */
          g_autoptr(RpmOstreeRefSack) rsack =
            rpmostree_get_refsack_for_root (ostree_sysroot_get_fd (sysroot),
                                            deployment_dirpath, cancellable, error);
          if (rsack == NULL)
            return FALSE;

          if (!add_package_refs_to_set (rsack, referenced_pkgs, cancellable, error))
            return FALSE;
        }
    }

  g_autoptr(GHashTable) current_refs = NULL;
  if (!ostree_repo_list_refs_ext (pkgcache_repo, "rpmostree/pkg", &current_refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable, error))
    return FALSE;

  guint n_freed = 0;
  GLNX_HASH_TABLE_FOREACH (current_refs, const char*, ref)
    {
      if (g_hash_table_contains (referenced_pkgs, ref))
        continue;

      if (!ostree_repo_set_ref_immediate (pkgcache_repo, NULL, ref, NULL,
                                          cancellable, error))
        return FALSE;
      n_freed++;
    }

  guint64 freed_space;
  gint n_objects_total, n_objects_pruned;
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
/* This is like ostree_sysroot_get_merge_deployment() except we explicitly
 * ignore the magical "booted" behavior. For rpm-ostree we're trying something
 * different now where we are a bit more stateful and pick up changes from the
 * pending root. This allows users to chain operations together naturally.
 */
OstreeDeployment *
rpmostree_syscore_get_origin_merge_deployment (OstreeSysroot *self, const char *osname)
{
  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (self);

  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];

      if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
        continue;

      return g_object_ref (deployment);
    }

  return NULL;
}


/* Copy of currently private _ostree_sysroot_bump_mtime()
 * until we decide to either formalize that, or have a method
 * to notify of changes to e.g. live replaced xattrs.
 */
gboolean
rpmostree_syscore_bump_mtime (OstreeSysroot   *sysroot,
                              GError         **error)
{
  if (utimensat (ostree_sysroot_get_fd (sysroot), "ostree/deploy", NULL, 0) < 0)
    return glnx_throw_errno_prefix (error, "futimens");
  return TRUE;
}

/* A version of ostree_sysroot_simple_write_deployment() but with
 * a few changes:
 *
 *  - There's just @pushing_rollback, which if true makes the deployment not-default
 *    as well as retaining the pending deployment
 *  - osname logic is based on new deployment
 *  - Fix insertion of deployment to be after booted (patch pending for ostree upstream)
 */
GPtrArray *
rpmostree_syscore_add_deployment (OstreeSysroot      *sysroot,
                                  OstreeDeployment   *new_deployment,
                                  OstreeDeployment   *merge_deployment,
                                  gboolean            pushing_rollback,
                                  GError            **error)
{
  OstreeDeployment *booted_deployment = NULL;
  g_autoptr(GPtrArray) deployments = NULL;
  g_autoptr(GPtrArray) new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  const char *osname = ostree_deployment_get_osname (new_deployment);
  /* Whether or not we added @new_deployment to the list yet */
  gboolean added_new = FALSE;
  /* Keep track of whether we're looking at a deployment before or after the booted */
  gboolean before_booted = TRUE;
  gboolean booted_is_live = FALSE;

  deployments = ostree_sysroot_get_deployments (sysroot);
  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  if (!pushing_rollback)
    {
      g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
      added_new = TRUE;
    }

  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      const gboolean osname_matches = (strcmp (ostree_deployment_get_osname (deployment), osname) == 0);
      const gboolean is_booted = ostree_deployment_equal (deployment, booted_deployment);
      const gboolean is_merge_or_booted = is_booted ||
        ostree_deployment_equal (deployment, merge_deployment);
      const gboolean is_last = (i == (deployments->len - 1));

      if (is_booted)
        {
          before_booted = FALSE;
          if (!rpmostree_syscore_deployment_is_live (sysroot, deployment, -1,
                                                     &booted_is_live, error))
            return NULL;
        }

      /* Retain deployment if:
       *   - The deployment is for another osname
       *   - We're pushing a rollback and this is a pending deployment
       *   - It's the merge or booted deployment
       *   - The booted deployment is live, this is a rollback
       */
      if (!osname_matches
          || (pushing_rollback && before_booted)
          || is_merge_or_booted
          || (!before_booted && booted_is_live))
        g_ptr_array_add (new_deployments, g_object_ref (deployment));

      /* Insert new rollback right after the booted */
      if (!added_new && (!before_booted || is_last))
        {
          g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
          added_new = TRUE;
        }
    }

  return g_steal_pointer (&new_deployments);
}

/* Find the pending and rollback deployments (if any) for @osname. */
void
rpmostree_syscore_query_deployments (OstreeSysroot      *sysroot,
                                     const char         *osname,
                                     OstreeDeployment  **out_pending,
                                     OstreeDeployment  **out_rollback)
{
  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  g_autoptr(OstreeDeployment) ret_pending = NULL;
  g_autoptr(OstreeDeployment) ret_rollback = NULL;

  gboolean found_booted = FALSE;
  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];

      /* Is this deployment booted?  If so, note we're past the booted */
      if (booted_deployment != NULL &&
          ostree_deployment_equal (deployment, booted_deployment))
        {
          found_booted = TRUE;
          continue;
        }

      /* Ignore deployments not for this osname */
      if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
          continue;

      if (!found_booted && !ret_pending)
        ret_pending = g_object_ref (deployment);

      if (found_booted && !ret_rollback)
        ret_rollback = g_object_ref (deployment);
    }
  if (out_pending)
    *out_pending = g_steal_pointer (&ret_pending);
  if (out_rollback)
    *out_rollback = g_steal_pointer (&ret_rollback);
}


/* Also a variant of ostree_sysroot_simple_write_deployment(), but here we are
 * just trying to remove a pending and/or rollback.
 */
GPtrArray *
rpmostree_syscore_filter_deployments (OstreeSysroot      *sysroot,
                                      const char         *osname,
                                      gboolean            cleanup_pending,
                                      gboolean            cleanup_rollback)
{
  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  g_autoptr(GPtrArray) new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  OstreeDeployment *booted_deployment = NULL;
  gboolean found_booted = FALSE;

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];

      /* Is this deployment booted?  If so, note we're past the booted,
       * and ensure it's added. */
      if (booted_deployment != NULL &&
          ostree_deployment_equal (deployment, booted_deployment))
        {
          found_booted = TRUE;
          g_ptr_array_add (new_deployments, g_object_ref (deployment));
          continue;
        }

      /* Is this deployment for a different osname?  Keep it. */
      if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
        {
          g_ptr_array_add (new_deployments, g_object_ref (deployment));
          continue;
        }

      /* Now, we may skip this deployment, i.e. GC it. */
      if (!found_booted && cleanup_pending)
        continue;

      if (found_booted && cleanup_rollback)
        continue;

      /* Otherwise, add it */
      g_ptr_array_add (new_deployments, g_object_ref (deployment));
    }

  if (new_deployments->len == deployments->len)
    return NULL;
  return g_steal_pointer (&new_deployments);
}

/* Commit @new_deployments and perform a cleanup */
gboolean
rpmostree_syscore_write_deployments (OstreeSysroot           *sysroot,
                                     OstreeRepo              *repo,
                                     GPtrArray               *new_deployments,
                                     GCancellable            *cancellable,
                                     GError                 **error)
{
  glnx_unref_object OstreeRepo *owned_repo = NULL;

  /* Allow the caller to pass NULL as a convenience; in the future we really
   * should have a strong ref to a repo in the sysroot and make retrieving it
   * not failable.
   */
  if (repo == NULL)
    {
      if (!ostree_sysroot_get_repo (sysroot, &owned_repo, cancellable, error))
        return FALSE;
      repo = owned_repo;
    }

  OstreeSysrootWriteDeploymentsOpts write_opts = { .do_postclean = FALSE };
  if (!ostree_sysroot_write_deployments_with_options (sysroot, new_deployments,
                                                      &write_opts, cancellable, error))
    return FALSE;

  if (!rpmostree_syscore_cleanup (sysroot, repo, cancellable, error))
    return FALSE;

  return TRUE;
}

/* Load the checksums that describe the "livefs" state of the given
 * deployment.
 */
gboolean
rpmostree_syscore_deployment_get_live (OstreeSysroot    *sysroot,
                                       OstreeDeployment *deployment,
                                       int               deployment_dfd,
                                       char            **out_inprogress_checksum,
                                       char            **out_livereplaced_checksum,
                                       GError          **error)
{
  g_autoptr(RpmOstreeOrigin) origin = rpmostree_origin_parse_deployment (deployment, error);
  if (!origin)
    return FALSE;
  rpmostree_origin_get_live_state (origin, out_inprogress_checksum, out_livereplaced_checksum);
  return TRUE;
}

/* Set @out_is_live to %TRUE if the deployment is live-modified */
gboolean
rpmostree_syscore_deployment_is_live (OstreeSysroot    *sysroot,
                                      OstreeDeployment *deployment,
                                      int               deployment_dfd,
                                      gboolean         *out_is_live,
                                      GError          **error)
{
  g_autofree char *inprogress_checksum = NULL;
  g_autofree char *livereplaced_checksum = NULL;

  if (!rpmostree_syscore_deployment_get_live (sysroot, deployment, deployment_dfd,
                                              &inprogress_checksum, &livereplaced_checksum,
                                              error))
    return FALSE;

  *out_is_live = (inprogress_checksum != NULL || livereplaced_checksum != NULL);
  return TRUE;
}
