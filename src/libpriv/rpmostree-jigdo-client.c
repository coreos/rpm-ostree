/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */


/* This file contains the client-side portions of jigdo that are "private"
 * implementation detials of RpmOstreeContext. A better model down the line
 * might be to have RpmOstreeJigdoContext or so.
 */

#include "config.h"

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include "rpmostree-jigdo-assembler.h"
#include "rpmostree-core-private.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-output.h"
// For the jigdo Requires parsing
#include <libdnf/dnf-reldep-private.hpp>
#include <libdnf/dnf-sack-private.hpp>

#include <string.h>
#include <stdlib.h>

static DnfPackage *
query_jigdo_pkg (DnfContext *dnfctx,
                 const char *name_arch,
                 const char *evr,
                 GError    **error)
{
  hy_autoquery HyQuery query = hy_query_create (dnf_context_get_sack (dnfctx));
  /* This changed in v4, we now go through the Provides: name(arch) */
  const char *arch_start = strchr (name_arch, '(');
  g_autofree char *name_owned = NULL;
  const char *name;
  if (arch_start)
    {
      name = name_owned = g_strndup (name_arch, arch_start - name_arch);
      hy_query_filter (query, HY_PKG_PROVIDES, HY_EQ, name_arch);
    }
  else
    name = name_arch;
  hy_query_filter (query, HY_PKG_NAME, HY_EQ, name);
  hy_query_filter (query, HY_PKG_EVR, HY_EQ, evr);
  g_autoptr(GPtrArray) pkglist = hy_query_run (query);
  if (pkglist->len == 0)
    return glnx_null_throw (error, "Failed to find package %s = %s", name_arch, evr);
  return g_object_ref (pkglist->pdata[0]);
}

static int
compare_pkgs (gconstpointer ap,
                      gconstpointer bp)
{
  DnfPackage **a = (gpointer)ap;
  DnfPackage **b = (gpointer)bp;
  return dnf_package_cmp (*a, *b);
}

/* Core logic for performing a jigdo assembly client side.  The high level flow is:
 *
 * - Download rpm-md
 * - query for jigdoRPM
 * - query for jigdoSet (dependencies of above)
 * - download and parse jigdoRPM
 * - download and import jigdoSet
 * - commit all data to ostree
 */
gboolean
rpmostree_context_execute_jigdo (RpmOstreeContext     *self,
                                 gboolean             *out_changed,
                                 GCancellable         *cancellable,
                                 GError              **error)
{
  OstreeRepo *repo = self->ostreerepo;
  DnfPackage* oirpm_pkg = rpmostree_context_get_jigdo_pkg (self);
  const char *provided_commit = rpmostree_context_get_jigdo_checksum (self);

  DnfContext *dnfctx = rpmostree_context_get_dnf (self);

  { OstreeRepoCommitState commitstate;
    gboolean has_commit;
    if (!ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_COMMIT, provided_commit,
                                 &has_commit, cancellable, error))
      return FALSE;
    if (has_commit)
      {
        if (!ostree_repo_load_commit (repo, provided_commit, NULL,
                                      &commitstate, error))
          return FALSE;
        if (!(commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL))
          {
            *out_changed = FALSE;
            return TRUE;  /* 🔚 Early return */
          }
      }
  }

  rpmostree_output_message ("Updating to: %s:%s", dnf_package_get_reponame (oirpm_pkg), dnf_package_get_nevra (oirpm_pkg));

  g_autoptr(GPtrArray) pkgs_required = g_ptr_array_new_with_free_func (g_object_unref);

  /* Look at the Requires of the jigdoRPM.  Note that we don't want to do
   * dependency resolution here - that's part of the whole idea, we're doing
   * deterministic imaging.
   */
  g_autoptr(DnfReldepList) requires = dnf_package_get_requires (oirpm_pkg);
  const gint n_requires = dnf_reldep_list_count (requires);
  Pool *pool = dnf_sack_get_pool (dnf_context_get_sack (dnfctx));
  for (int i = 0; i < n_requires; i++)
    {
      DnfReldep *req = dnf_reldep_list_index (requires, i);
      Id reqid = dnf_reldep_get_id (req);
      if (!ISRELDEP (reqid))
        continue;
      Reldep *rdep = GETRELDEP (pool, reqid);
      /* This is the core hack; we're searching for Requires that have exact '='
       * versions. This assumes that the rpmbuild process won't inject such
       * requirements.
       */
      if (!(rdep->flags & REL_EQ))
        continue;

      /* Since v4 the server uses "Provides: name(arch) for archful */
      const char *name_arch = pool_id2str (pool, rdep->name);
      const char *evr = pool_id2str (pool, rdep->evr);

      DnfPackage *pkg = query_jigdo_pkg (dnfctx, name_arch, evr, error);
      // FIXME: Possibly we shouldn't require a package to be in the repos if we
      // already have it imported? This would help support downgrades if the
      // repo owner has pruned.
      if (!pkg)
        return FALSE;
      g_ptr_array_add (pkgs_required, g_object_ref (pkg));
    }
  g_ptr_array_sort (pkgs_required, compare_pkgs);

  /* For now we first serially download the oirpm, but down the line we can do
   * this async. Doing so will require putting more of the jigdo logic into the
   * core, so it knows not to import the jigdoRPM.
   */
  { g_autoptr(GPtrArray) oirpm_singleton_pkglist = g_ptr_array_new ();
    g_ptr_array_add (oirpm_singleton_pkglist, oirpm_pkg);
    if (!rpmostree_context_set_packages (self, oirpm_singleton_pkglist, cancellable, error))
      return FALSE;
  }

  if (!rpmostree_context_download (self, cancellable, error))
    return FALSE;

  glnx_fd_close int oirpm_fd = -1;
  if (!rpmostree_context_consume_package (self, oirpm_pkg, &oirpm_fd, error))
    return FALSE;

  g_autoptr(RpmOstreeJigdoAssembler) jigdo = rpmostree_jigdo_assembler_new_take_fd (&oirpm_fd, oirpm_pkg, error);
  if (!jigdo)
    return FALSE;
  g_autofree char *checksum = NULL;
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) commit_meta = NULL;
  if (!rpmostree_jigdo_assembler_read_meta (jigdo, &checksum, &commit, &commit_meta,
                                            cancellable, error))
    return FALSE;

  if (!g_str_equal (checksum, provided_commit))
    return glnx_throw (error, "Package '%s' commit mismatch; Provides=%s, actual=%s",
                       dnf_package_get_nevra (oirpm_pkg), provided_commit, checksum);

  g_printerr ("TODO implement GPG verification\n");

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  if (!rpmostree_repo_auto_transaction_start (&txn, repo, FALSE, cancellable, error))
    return FALSE;

  if (!ostree_repo_write_commit_detached_metadata (repo, checksum, commit_meta,
                                                   cancellable, error))
    return FALSE;
  /* Mark as partial until we're done */
  if (!ostree_repo_mark_commit_partial (repo, checksum, TRUE, error))
    return FALSE;
  { g_autofree guint8*csum = NULL;
    if (!ostree_repo_write_metadata (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     checksum, commit, &csum,
                                     cancellable, error))
      return FALSE;
  }

  if (!rpmostree_jigdo_assembler_write_new_objects (jigdo, repo, cancellable, error))
    return FALSE;

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;
  txn.initialized = FALSE;

  /* And now, process the jigdo set */
  if (!rpmostree_context_set_packages (self, pkgs_required, cancellable, error))
    return FALSE;

  /* See what packages we need to import, print their size. TODO clarify between
   * download/import.
   */
  g_autoptr(GHashTable) pkgset_to_import = g_hash_table_new (NULL, NULL);
  { g_autoptr(GPtrArray) pkgs_to_import = rpmostree_context_get_packages_to_import (self);
    guint64 dlsize = 0;
    for (guint i = 0; i < pkgs_to_import->len; i++)
      {
        DnfPackage *pkg = pkgs_to_import->pdata[i];
        dlsize += dnf_package_get_size (pkg);
        g_hash_table_add (pkgset_to_import, pkg);
      }
    g_autofree char *dlsize_fmt = g_format_size (dlsize);
    rpmostree_output_message ("%u packages to import, download size: %s", pkgs_to_import->len, dlsize_fmt);
  }

  /* Parse the xattr data in the jigdoRPM */
  g_autoptr(GHashTable) pkg_to_xattrs = g_hash_table_new_full (NULL, NULL,
                                                               (GDestroyNotify)g_object_unref,
                                                               (GDestroyNotify)g_variant_unref);

  for (guint i = 0; i < pkgs_required->len; i++)
    {
      DnfPackage *pkg = pkgs_required->pdata[i];
      const gboolean should_import = g_hash_table_contains (pkgset_to_import, pkg);
      g_autoptr(GVariant) objid_to_xattrs = NULL;
      if (!rpmostree_jigdo_assembler_next_xattrs (jigdo, &objid_to_xattrs, cancellable, error))
        return FALSE;
      if (!objid_to_xattrs)
        return glnx_throw (error, "missing xattr entry: %s", dnf_package_get_name (pkg));
      if (!should_import)
        continue;
      g_hash_table_insert (pkg_to_xattrs, g_object_ref (pkg), g_steal_pointer (&objid_to_xattrs));
    }

  /* Start the download and import, using the xattr data from the jigdoRPM */
  if (!rpmostree_context_download (self, cancellable, error))
    return FALSE;
  g_autoptr(GVariant) xattr_table = rpmostree_jigdo_assembler_get_xattr_table (jigdo);
  if (!rpmostree_context_import_jigdo (self, xattr_table, pkg_to_xattrs,
                                       cancellable, error))
    return FALSE;

  /* Last thing is to delete the partial marker, just like
   * ostree_repo_pull_with_options().
   */
  if (!ostree_repo_mark_commit_partial (repo, checksum, FALSE, error))
    return FALSE;

  *out_changed = TRUE;

  return TRUE;
}

