/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <systemd/sd-journal.h>
#include <libglnx.h>

#include "rpmostree-types.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostree-origin.h"
#include "rpmostree-core.h"
#include "rpmostree-util.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-sysroot-core.h"
#include "rpmostree-core.h"
#include "rpmostree-package-variants.h"
#include "rpmostreed-utils.h"
#include "rpmostreed-errors.h"

/* Get a currently unique (for this host) identifier for the
 * deployment; TODO - adding the deployment timestamp would make it
 * persistently unique, needs API in libostree.
 */
char *
rpmostreed_deployment_generate_id (OstreeDeployment *deployment)
{
  g_return_val_if_fail (OSTREE_IS_DEPLOYMENT (deployment), NULL);
  return g_strdup_printf ("%s-%s.%u",
                          ostree_deployment_get_osname (deployment),
                          ostree_deployment_get_csum (deployment),
                          ostree_deployment_get_deployserial (deployment));
}

OstreeDeployment *
rpmostreed_deployment_get_for_id (OstreeSysroot *sysroot,
                                  const gchar *deploy_id)
{
  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  for (guint i = 0; i < deployments->len; i++)
    {
      g_autofree gchar *id = rpmostreed_deployment_generate_id (deployments->pdata[i]);
      if (g_strcmp0 (deploy_id, id) == 0)
        return g_object_ref (deployments->pdata[i]);
    }

  return NULL;
}


/* rpmostreed_deployment_get_for_index:
 *
 * sysroot: A #OstreeSysroot instance
 * index: string index being parsed
 * error: a #GError instance
 *
 * Get a deployment based on a string index,
 * the string is parsed and checked. Then
 * the deployment at the parsed index will be
 * returned.
 *
 * returns: NULL if an error is being made
 */
OstreeDeployment *
rpmostreed_deployment_get_for_index (OstreeSysroot *sysroot,
                                     const gchar   *index,
                                     GError       **error)
{
  g_autoptr(GError) local_error = NULL;
  int deployment_index = -1;
  for (int counter = 0; counter < strlen(index); counter++)
    {
      if (!g_ascii_isdigit (index[counter]))
        {
          local_error = g_error_new (RPM_OSTREED_ERROR,
                                     RPM_OSTREED_ERROR_FAILED,
                                     "Invalid deployment index %s, must be a number and >= 0",
                                     index);
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }
    }
  deployment_index = atoi (index);

  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployment_index >= deployments->len)
    {
      local_error = g_error_new (RPM_OSTREED_ERROR,
                                 RPM_OSTREED_ERROR_FAILED,
                                 "Out of range deployment index %d, expected < %d",
                                 deployment_index, deployments->len);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }
  return g_object_ref (deployments->pdata[deployment_index]);
}

static gboolean
rpmostreed_deployment_gpg_results (OstreeRepo  *repo,
                                   const gchar *origin_refspec,
                                   const gchar *checksum,
                                   GVariant   **out_results,
                                   gboolean    *out_enabled,
                                   GError     **error)
{
  GLNX_AUTO_PREFIX_ERROR ("GPG verification error", error);

  g_autofree gchar *remote = NULL;
  if (!ostree_parse_refspec (origin_refspec, &remote, NULL, error))
    return FALSE;

  gboolean gpg_verify = FALSE;
  if (remote)
    {
      if (!ostree_repo_remote_get_gpg_verify (repo, remote, &gpg_verify, error))
        return FALSE;
    }

  if (!gpg_verify)
    {
      /* Note early return; no need to verify signatures! */
      *out_enabled = FALSE;
      *out_results = NULL;
      return TRUE;
    }

  g_autoptr(GError) local_error = NULL;
  g_autoptr(OstreeGpgVerifyResult) verify_result =
    ostree_repo_verify_commit_for_remote (repo, checksum, remote, NULL, &local_error);
  if (!verify_result)
    {
      /* Somehow, we have a deployment which has gpg-verify=true, but *doesn't* have a valid
       * signature. Let's not just bomb out here. We need to return this in the variant so
       * that `status` can show the appropriate msg. */
      *out_enabled = TRUE;
      *out_results = NULL;
      return TRUE;
    }

  g_auto(GVariantBuilder) builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));

  guint n_sigs = ostree_gpg_verify_result_count_all (verify_result);
  for (guint i = 0; i < n_sigs; i++)
    g_variant_builder_add (&builder, "v", ostree_gpg_verify_result_get_all (verify_result, i));

  *out_results = g_variant_ref_sink (g_variant_builder_end (&builder));
  *out_enabled = TRUE;
  return TRUE;
}

GVariant *
rpmostreed_deployment_generate_blank_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);

  return g_variant_dict_end (&dict);
}

static void
variant_add_metadata_attribute (GVariantDict *dict,
                                const gchar  *attribute,
                                const gchar  *new_attribute,
                                GVariant     *commit)
{
  g_autofree gchar *attribute_string_value = NULL;
  g_autoptr(GVariant) metadata = g_variant_get_child_value (commit, 0);

  if (metadata != NULL)
    {
      g_variant_lookup (metadata, attribute, "s", &attribute_string_value);
      if (attribute_string_value != NULL)
        g_variant_dict_insert (dict, new_attribute ?: attribute, "s", attribute_string_value);
    }
}

static void
variant_add_commit_details (GVariantDict *dict,
                            const char   *prefix,
                            GVariant     *commit)
{
  g_autoptr(GVariant) metadata = NULL;
  g_autofree gchar *version_commit = NULL;
  guint64 timestamp = 0;

  timestamp = ostree_commit_get_timestamp (commit);
  metadata = g_variant_get_child_value (commit, 0);
  if (metadata != NULL)
    g_variant_lookup (metadata, "version", "s", &version_commit);

  if (version_commit != NULL)
    g_variant_dict_insert (dict, glnx_strjoina (prefix ?: "", "version"),
                           "s", version_commit);
  if (timestamp > 0)
    g_variant_dict_insert (dict, glnx_strjoina (prefix ?: "", "timestamp"),
                           "t", timestamp);
}

static void
variant_add_from_hash_table (GVariantDict *dict,
                             const char   *key,
                             GHashTable   *table)
{
  g_autofree char **values = (char**)g_hash_table_get_keys_as_array (table, NULL);
  g_variant_dict_insert (dict, key, "^as", values);
}

GVariant *
rpmostreed_deployment_generate_variant (OstreeSysroot *sysroot,
                                        OstreeDeployment *deployment,
                                        const char *booted_id,
                                        OstreeRepo *repo,
                                        GError **error)
{
  const gchar *osname = ostree_deployment_get_osname (deployment);
  const gchar *csum = ostree_deployment_get_csum (deployment);
  gint serial = ostree_deployment_get_deployserial (deployment);

  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo,
                                 OSTREE_OBJECT_TYPE_COMMIT,
                                 csum,
                                 &commit,
                                 error))
    return NULL;

  g_autofree gchar *id = rpmostreed_deployment_generate_id (deployment);
  g_autoptr(RpmOstreeOrigin) origin = rpmostree_origin_parse_deployment (deployment, error);
  if (!origin)
    return NULL;

  RpmOstreeRefspecType refspec_type;
  g_autofree char *refspec = rpmostree_origin_get_full_refspec (origin, &refspec_type);

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);

  g_variant_dict_insert (&dict, "id", "s", id);
  if (osname != NULL)
    g_variant_dict_insert (&dict, "osname", "s", osname);
  g_variant_dict_insert (&dict, "serial", "i", serial);
  g_variant_dict_insert (&dict, "checksum", "s", csum);

  gboolean is_layered = FALSE;
  g_autofree char *base_checksum = NULL;
  g_auto(GStrv) layered_pkgs = NULL;
  g_autoptr(GVariant) removed_base_pkgs = NULL;
  g_autoptr(GVariant) replaced_base_pkgs = NULL;
  if (!rpmostree_deployment_get_layered_info (repo, deployment, &is_layered, &base_checksum,
                                              &layered_pkgs, &removed_base_pkgs,
                                              &replaced_base_pkgs, error))
    return NULL;

  g_autoptr(GVariant) base_commit = NULL;
  if (is_layered)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     base_checksum, &base_commit, error))
        return NULL;

      g_variant_dict_insert (&dict, "base-checksum", "s", base_checksum);
      variant_add_commit_details (&dict, "base-", base_commit);
      /* for layered commits, check if their base commit has end of life attribute */
      variant_add_metadata_attribute (&dict, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "endoflife", base_commit);

      /* See below for base commit metadata */
      g_autoptr(GVariant) layered_metadata = g_variant_get_child_value (commit, 0);
      g_variant_dict_insert (&dict, "layered-commit-meta", "@a{sv}", layered_metadata);
    }
  else
    {
      base_commit = g_variant_ref (commit);
      base_checksum = g_strdup (csum);
      variant_add_metadata_attribute (&dict, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "endoflife", commit);
    }

  /* We used to bridge individual keys, but that was annoying; just pass through all
   * of the commit metadata.
   */
  { g_autoptr(GVariant) base_meta = g_variant_get_child_value (commit, 0);
    g_variant_dict_insert (&dict, "base-commit-meta", "@a{sv}", base_meta);
  }
  variant_add_commit_details (&dict, NULL, commit);

  gboolean gpg_enabled = FALSE;
  g_autoptr(GVariant) sigs = NULL;
  if (refspec_type == RPMOSTREE_REFSPEC_TYPE_OSTREE)
    {
      if (!rpmostreed_deployment_gpg_results (repo, refspec, base_checksum, &sigs, &gpg_enabled, error))
        return NULL;

      g_autofree char *pending_base_commitrev = NULL;
      if (!ostree_repo_resolve_rev (repo, refspec, TRUE,
                                    &pending_base_commitrev, error))
        return NULL;

      if (pending_base_commitrev && !g_str_equal (pending_base_commitrev, base_checksum))
        {
          g_autoptr(GVariant) pending_base_commit = NULL;

          if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                         pending_base_commitrev, &pending_base_commit,
                                         error))
            return NULL;

          g_variant_dict_insert (&dict, "pending-base-checksum", "s", pending_base_commitrev);
          variant_add_commit_details (&dict, "pending-base-", pending_base_commit);
        }
    }

  g_autofree char *live_inprogress = NULL;
  g_autofree char *live_replaced = NULL;
  if (!rpmostree_syscore_deployment_get_live (sysroot, deployment, &live_inprogress,
                                              &live_replaced, error))
    return NULL;

  if (live_inprogress)
    g_variant_dict_insert (&dict, "live-inprogress", "s", live_inprogress);
  if (live_replaced)
    g_variant_dict_insert (&dict, "live-replaced", "s", live_replaced);

  if (refspec)
    g_variant_dict_insert (&dict, "origin", "s", refspec);

  variant_add_from_hash_table (&dict, "requested-packages",
                               rpmostree_origin_get_packages (origin));
  variant_add_from_hash_table (&dict, "requested-local-packages",
                               rpmostree_origin_get_local_packages (origin));
  variant_add_from_hash_table (&dict, "requested-base-removals",
                               rpmostree_origin_get_overrides_remove (origin));
  variant_add_from_hash_table (&dict, "requested-base-local-replacements",
                               rpmostree_origin_get_overrides_local_replace (origin));

  g_variant_dict_insert (&dict, "packages", "^as", layered_pkgs);
  g_variant_dict_insert_value (&dict, "base-removals", removed_base_pkgs);
  g_variant_dict_insert_value (&dict, "base-local-replacements", replaced_base_pkgs);

  if (sigs != NULL)
    g_variant_dict_insert_value (&dict, "signatures", sigs);
  g_variant_dict_insert (&dict, "gpg-enabled", "b", gpg_enabled);

  g_variant_dict_insert (&dict, "unlocked", "s",
                         ostree_deployment_unlocked_state_to_string (ostree_deployment_get_unlocked (deployment)));

  g_variant_dict_insert (&dict, "regenerate-initramfs", "b",
                         rpmostree_origin_get_regenerate_initramfs (origin));
  { const char *const* args = rpmostree_origin_get_initramfs_args (origin);
    if (args && *args)
      g_variant_dict_insert (&dict, "initramfs-args", "^as", args);
  }

  if (booted_id != NULL)
    g_variant_dict_insert (&dict, "booted", "b", g_strcmp0 (booted_id, id) == 0);

  return g_variant_dict_end (&dict);
}

static gboolean
add_all_commit_details_to_vardict (OstreeDeployment *deployment,
                                   OstreeRepo       *repo,
                                   const char       *refspec,  /* allow-none */
                                   const char       *checksum, /* allow-none */
                                   GVariant         *commit,   /* allow-none */
                                   GVariantDict     *dict,     /* allow-none */
                                   GError          **error)
{
  const gchar *osname = ostree_deployment_get_osname (deployment);

  g_autofree gchar *refspec_owned = NULL;
  gboolean refspec_is_ostree = FALSE;
  if (!refspec)
    {
      g_autoptr(RpmOstreeOrigin) origin =
        rpmostree_origin_parse_deployment (deployment, error);
      if (!origin)
        return FALSE;
      RpmOstreeRefspecType refspec_type;
      refspec = refspec_owned = rpmostree_origin_get_full_refspec (origin, &refspec_type);
      refspec_is_ostree = (refspec_type == RPMOSTREE_REFSPEC_TYPE_OSTREE);
    }

  g_assert (refspec);

  /* allow_noent=TRUE since the ref may have been deleted for a rebase */
  g_autofree gchar *checksum_owned = NULL;
  if (!checksum)
    {
      if (refspec_is_ostree)
        {
          if (!ostree_repo_resolve_rev (repo, refspec, TRUE, &checksum_owned, error))
            return FALSE;
        }

      /* in that case, use deployment csum */
      checksum = checksum_owned ?: ostree_deployment_get_csum (deployment);
    }

  g_autoptr(GVariant) commit_owned = NULL;
  if (!commit)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                     &commit_owned, error))
        return FALSE;
      commit = commit_owned;
    }

  gboolean gpg_enabled = FALSE;
  g_autoptr(GVariant) sigs = NULL;
  if (refspec_is_ostree)
    {
      if (!rpmostreed_deployment_gpg_results (repo, refspec, checksum,
                                              &sigs, &gpg_enabled, error))
        return FALSE;
    }

  if (osname != NULL)
    g_variant_dict_insert (dict, "osname", "s", osname);
  g_variant_dict_insert (dict, "checksum", "s", checksum);
  variant_add_commit_details (dict, NULL, commit);
  g_variant_dict_insert (dict, "origin", "s", refspec);
  if (sigs != NULL)
    g_variant_dict_insert_value (dict, "signatures", sigs);
  g_variant_dict_insert (dict, "gpg-enabled", "b", gpg_enabled);
  return TRUE;
}

GVariant *
rpmostreed_commit_generate_cached_details_variant (OstreeDeployment *deployment,
                                                   OstreeRepo *repo,
                                                   const gchar *refspec,
                                                   GError **error)
{
  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  if (!add_all_commit_details_to_vardict (deployment, repo, refspec,
                                          NULL, NULL, &dict, error))
    return NULL;

  return g_variant_ref_sink (g_variant_dict_end (&dict));
}

typedef struct {
  gboolean   initialized;
  GPtrArray *upgraded;
  GPtrArray *downgraded;
  GPtrArray *removed;
  GPtrArray *added;
} RpmDiff;

static void
rpm_diff_init (RpmDiff *diff)
{
  g_assert (!diff->initialized);
  diff->upgraded = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  diff->downgraded = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  diff->removed = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  diff->added = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  diff->initialized = TRUE;
}

static void
rpm_diff_clear (RpmDiff *diff)
{
  if (!diff->initialized)
    return;
  g_clear_pointer (&diff->upgraded, (GDestroyNotify)g_ptr_array_unref);
  g_clear_pointer (&diff->downgraded, (GDestroyNotify)g_ptr_array_unref);
  g_clear_pointer (&diff->removed, (GDestroyNotify)g_ptr_array_unref);
  g_clear_pointer (&diff->added, (GDestroyNotify)g_ptr_array_unref);
  diff->initialized = FALSE;
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (RpmDiff, rpm_diff_clear);

static GVariant*
single_pkg_variant_new (RpmOstreePkgTypes type,
                        RpmOstreePackage *pkg)
{
  return g_variant_ref_sink (
      g_variant_new ("(usss)", type,
                     rpm_ostree_package_get_name (pkg),
                     rpm_ostree_package_get_evr (pkg),
                     rpm_ostree_package_get_arch (pkg)));
}

static GVariant*
modified_pkg_variant_new (RpmOstreePkgTypes type,
                          RpmOstreePackage *pkg_old,
                          RpmOstreePackage *pkg_new)
{
  const char *name_old = rpm_ostree_package_get_name (pkg_old);
  const char *name_new = rpm_ostree_package_get_name (pkg_new);
  g_assert_cmpstr (name_old, ==, name_new);
  return g_variant_ref_sink (
      g_variant_new ("(us(ss)(ss))", type, name_old,
                     rpm_ostree_package_get_evr (pkg_old),
                     rpm_ostree_package_get_arch (pkg_old),
                     rpm_ostree_package_get_evr (pkg_new),
                     rpm_ostree_package_get_arch (pkg_new)));
}

static GVariant*
modified_dnfpkg_variant_new (RpmOstreePkgTypes type,
                             RpmOstreePackage *pkg_old,
                             DnfPackage       *pkg_new)
{
  const char *name_old = rpm_ostree_package_get_name (pkg_old);
  const char *name_new = dnf_package_get_name (pkg_new);
  g_assert_cmpstr (name_old, ==, name_new);
  return g_variant_ref_sink (
      g_variant_new ("(us(ss)(ss))", type, name_old,
                     rpm_ostree_package_get_evr (pkg_old),
                     rpm_ostree_package_get_arch (pkg_old),
                     dnf_package_get_evr (pkg_new),
                     dnf_package_get_arch (pkg_new)));
}

static void
rpm_diff_add_base_db_diff (RpmDiff    *diff,
                           /* element-type RpmOstreePackage */
                           GPtrArray  *removed,
                           GPtrArray  *added,
                           GPtrArray  *modified_old,
                           GPtrArray  *modified_new)
{
  g_assert_cmpuint (modified_old->len, ==, modified_new->len);

  RpmOstreePkgTypes type = RPM_OSTREE_PKG_TYPE_BASE;
  for (guint i = 0; i < removed->len; i++)
    g_ptr_array_add (diff->removed, single_pkg_variant_new (type, removed->pdata[i]));
  for (guint i = 0; i < added->len; i++)
    g_ptr_array_add (diff->added, single_pkg_variant_new (type, added->pdata[i]));
  for (guint i = 0; i < modified_old->len; i++)
    {
      RpmOstreePackage *old_pkg = modified_old->pdata[i];
      RpmOstreePackage *new_pkg = modified_new->pdata[i];
      if (rpm_ostree_package_cmp (old_pkg, new_pkg) < 0)
        g_ptr_array_add (diff->upgraded,
                         modified_pkg_variant_new (type, old_pkg, new_pkg));
      else
        g_ptr_array_add (diff->downgraded,
                         modified_pkg_variant_new (type, old_pkg, new_pkg));
    }
}

static void
rpm_diff_add_layered_diff (RpmDiff  *diff,
                           RpmOstreePackage *old_pkg,
                           DnfPackage       *new_pkg)
{
  /* add to upgraded; layered pkgs only go up */
  RpmOstreePkgTypes type = RPM_OSTREE_PKG_TYPE_LAYER;
  g_ptr_array_add (diff->upgraded, modified_dnfpkg_variant_new (type, old_pkg, new_pkg));
}

static int
sort_pkgvariant_by_name (gconstpointer  pkga_pp,
                         gconstpointer  pkgb_pp)
{
  GVariant *pkg_a = *((GVariant**)pkga_pp);
  GVariant *pkg_b = *((GVariant**)pkgb_pp);

  const char *pkgname_a;
  g_variant_get_child (pkg_a, 1, "&s", &pkgname_a);
  const char *pkgname_b;
  g_variant_get_child (pkg_b, 1, "&s", &pkgname_b);

  return strcmp (pkgname_a, pkgname_b);
}

static GVariant*
array_to_variant_new (const char *format, GPtrArray *array)
{
  if (array->len == 0)
    return g_variant_new (format, NULL);

  /* make doubly sure it's sorted */
  g_ptr_array_sort (array, sort_pkgvariant_by_name);

  g_auto(GVariantBuilder) builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE_ARRAY);
  for (guint i = 0; i < array->len; i++)
    g_variant_builder_add_value (&builder, array->pdata[i]);
  return g_variant_builder_end (&builder);
}

static gboolean
rpm_diff_is_empty (RpmDiff *diff)
{
  g_assert (diff->initialized);
  return !diff->upgraded->len &&
         !diff->downgraded->len &&
         !diff->removed->len &&
         !diff->added->len;
}

static GVariant*
rpm_diff_variant_new (RpmDiff *diff)
{
  g_assert (diff->initialized);
  g_auto(GVariantDict) dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert_value (&dict, "upgraded", array_to_variant_new (
                                 RPMOSTREE_DIFF_MODIFIED_GVARIANT_STRING, diff->upgraded));
  g_variant_dict_insert_value (&dict, "downgraded", array_to_variant_new (
                                 RPMOSTREE_DIFF_MODIFIED_GVARIANT_STRING, diff->downgraded));
  g_variant_dict_insert_value (&dict, "removed", array_to_variant_new (
                                 RPMOSTREE_DIFF_SINGLE_GVARIANT_STRING, diff->removed));
  g_variant_dict_insert_value (&dict, "added", array_to_variant_new (
                                 RPMOSTREE_DIFF_SINGLE_GVARIANT_STRING, diff->added));
  return g_variant_dict_end (&dict);
}

static DnfPackage*
find_newer_package (DnfSack    *sack,
                    RpmOstreePackage *pkg)
{
  hy_autoquery HyQuery query = hy_query_create (sack);
  hy_query_filter (query, HY_PKG_NAME, HY_EQ, rpm_ostree_package_get_name (pkg));
  hy_query_filter (query, HY_PKG_EVR, HY_GT, rpm_ostree_package_get_evr (pkg));
  hy_query_filter (query, HY_PKG_ARCH, HY_NEQ, "src");
  hy_query_filter_latest (query, TRUE);
  g_autoptr(GPtrArray) new_pkgs = hy_query_run (query);
  if (new_pkgs->len == 0)
    return NULL; /* canonicalize to NULL */
  g_ptr_array_sort (new_pkgs, (GCompareFunc)rpmostree_pkg_array_compare);
  return g_object_ref (new_pkgs->pdata[new_pkgs->len-1]);
}

/* For all layered pkgs, check if there are newer versions in the rpmmd. Add diff to
 * @rpm_diff, and all new pkgs in @out_newer_packages (these are used later for advisories).
 * */
static gboolean
rpmmd_diff (OstreeSysroot    *sysroot,
            /* these are just to avoid refetching them */
            OstreeRepo       *repo,
            OstreeDeployment *deployment,
            const char       *base_checksum,
            DnfSack          *sack,
            RpmDiff          *rpm_diff,
            GPtrArray       **out_newer_packages,
            GError          **error)
{
  /* Note here that we *don't* actually use layered_pkgs; we want to look at all the RPMs
   * installed, whereas the layered pkgs (actually patterns) just represent top-level
   * entries. IOW, we want to run through all layered RPMs, which include deps of
   * layered_pkgs. */

  g_autoptr(GPtrArray) all_layered_pkgs = NULL;
  const char *layered_checksum = ostree_deployment_get_csum (deployment);
  RpmOstreeDbDiffExtFlags flags = RPM_OSTREE_DB_DIFF_EXT_ALLOW_NOENT;
  if (!rpm_ostree_db_diff_ext (repo, base_checksum, layered_checksum, flags, NULL,
                               &all_layered_pkgs, NULL, NULL, NULL, error))
    return FALSE;

  /* XXX: need to filter out local pkgs; though we still want to check for advisories --
   * maybe we should do this in status.c instead? */

  if (all_layered_pkgs == NULL || /* -> older layer before we injected pkglist metadata */
      all_layered_pkgs->len == 0) /* -> no layered pkgs, e.g. override remove only */
    {
      *out_newer_packages = NULL;
      return TRUE; /* note early return */
    }

  /* for each layered pkg, check if there's a newer version available (in reality, there may
   * be other new pkgs that need to be layered or some pkgs that no longer need to, but we
   * won't find out until we have the full commit available -- XXX: we could go the extra
   * effort and use the rpmdb of new_checksum if we already have it somehow, though that's
   * probably not the common case */

  g_autoptr(GPtrArray) newer_packages =
    g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  for (guint i = 0; i < all_layered_pkgs->len; i++)
    {
      RpmOstreePackage *pkg = all_layered_pkgs->pdata[i];
      g_autoptr(DnfPackage) newer_pkg = find_newer_package (sack, pkg);
      if (!newer_pkg)
        continue;

      g_ptr_array_add (newer_packages, g_object_ref (newer_pkg));
      rpm_diff_add_layered_diff (rpm_diff, pkg, newer_pkg);
    }

  /* canonicalize to NULL if there's nothing new */
  if (newer_packages->len == 0)
    g_clear_pointer (&newer_packages, (GDestroyNotify)g_ptr_array_unref);

  *out_newer_packages = g_steal_pointer (&newer_packages);
  return TRUE;
}

static gboolean
get_cached_rpmmd_sack (OstreeSysroot     *sysroot,
                       OstreeRepo        *repo,
                       OstreeDeployment  *deployment,
                       DnfSack          **out_sack,
                       GError           **error)
{
  /* we don't need the full force of the core ctx here; we just want a DnfContext so that it
   * can load the repos and deal with releasever for us */
  g_autoptr(DnfContext) ctx = dnf_context_new ();

  /* We have to point to the same source root for releasever to hit the right cache: an
   * interesting point here is that if there's a newer $releasever pending (i.e. 'deploy'
   * auto update policy), we'll still be using the previous releasever -- this is OK though,
   * we should be special-casing these rebases later re. how to display them; at least
   * status already shows endoflife. See also deploy_transaction_execute(). */
  g_autofree char *deployment_root = rpmostree_get_deployment_root (sysroot, deployment);
  dnf_context_set_source_root (ctx, deployment_root);
  g_autofree char *reposdir = g_build_filename (deployment_root, "etc/yum.repos.d", NULL);
  dnf_context_set_repo_dir (ctx, reposdir);
  dnf_context_set_cache_dir (ctx, RPMOSTREE_CORE_CACHEDIR RPMOSTREE_DIR_CACHE_REPOMD);
  dnf_context_set_solv_dir (ctx, RPMOSTREE_CORE_CACHEDIR RPMOSTREE_DIR_CACHE_SOLV);

  if (!dnf_context_setup (ctx, NULL, error))
    return FALSE;

  /* add the repos but strictly from cache; we should have already *just* checked &
   * refreshed metadata as part of the DeployTransaction; but we gracefully handle bad cache
   * too (e.g. if we start using the new dnf_context_clean_cache() on rebases?) */
  GPtrArray *repos = dnf_context_get_repos (ctx);

  /* need a new DnfSackAddFlags flag for dnf_sack_add_repos to say "don't fallback to
   * updating if cache invalid/absent"; for now, just do it ourselves */
  GPtrArray *cached_enabled_repos = g_ptr_array_new ();
  for (guint i = 0; i < repos->len; i++)
    {
      DnfRepo *repo = repos->pdata[i];
      if ((dnf_repo_get_enabled (repo) & DNF_REPO_ENABLED_PACKAGES) == 0)
        continue;

      /* TODO: We need to expand libdnf here to somehow do a dnf_repo_check() without it
       * triggering a download if there's no cache at all. Here, we just physically check
       * for the location. */
      const char *location = dnf_repo_get_location (repo);
      if (!glnx_fstatat_allow_noent (AT_FDCWD, location, NULL, 0, error))
        return FALSE;
      if (errno == ENOENT)
        continue;

      g_autoptr(GError) local_error = NULL;
      g_autoptr(DnfState) state = dnf_state_new ();
      if (!dnf_repo_check (repo, G_MAXUINT, state, &local_error))
        sd_journal_print (LOG_WARNING, "Couldn't load cache for repo %s: %s",
                          dnf_repo_get_id (repo), local_error->message);
      else
        g_ptr_array_add (cached_enabled_repos, repo);
    }

  g_autoptr(DnfSack) sack = NULL;
  if (cached_enabled_repos->len > 0)
    {
      /* Set up our own sack and point it to the solv cache. TODO: We could've used the sack
       * from dnf_context_setup_sack(), but we need to extend libdnf to specify flags like
       * UPDATEINFO beforehand. Otherwise we have to add_repos() twice which almost double
       * startup time. */
      sack = dnf_sack_new ();
      dnf_sack_set_cachedir (sack, RPMOSTREE_CORE_CACHEDIR RPMOSTREE_DIR_CACHE_SOLV);
      if (!dnf_sack_setup (sack, DNF_SACK_SETUP_FLAG_MAKE_CACHE_DIR, error))
        return FALSE;

      /* we still use add_repos rather than add_repo separately above because it does nice
       * things like process excludes */
      g_autoptr(DnfState) state = dnf_state_new ();
      if (!dnf_sack_add_repos (sack, cached_enabled_repos, G_MAXUINT,
                               DNF_SACK_ADD_FLAG_UPDATEINFO, state, error))
        return FALSE;
    }

  *out_sack = g_steal_pointer (&sack);
  return TRUE;
}

/* The variant returned by this function is backwards compatible with the one returned by
 * rpmostreed_commit_generate_cached_details_variant(). However, it also includes a base
 * tree db diff, layered pkgs diff, state, advisories, etc... Also, it will happily return
 * NULL if no updates are available. */
gboolean
rpmostreed_update_generate_variant (OstreeSysroot *sysroot,
                                    OstreeDeployment *deployment,
                                    OstreeRepo *repo,
                                    GVariant **out_update,
                                    GError **error)
{
  /* We try to minimize I/O in this function. We're in the daemon startup path, and thus
   * directly contribute to lag from a cold `rpm-ostree status`. Anyway, as a principle we
   * shouldn't do long-running operations outside of transactions. */

  g_autoptr(RpmOstreeOrigin) origin = rpmostree_origin_parse_deployment (deployment, error);
  if (!origin)
    return FALSE;

  const char *refspec = rpmostree_origin_get_refspec (origin);
  { RpmOstreeRefspecType refspectype = RPMOSTREE_REFSPEC_TYPE_OSTREE;
    const char *refspec_data;
    if (!rpmostree_refspec_classify (refspec, &refspectype, &refspec_data, error))
      return FALSE;

    /* we don't support jigdo-based origins yet */
    if (refspectype != RPMOSTREE_REFSPEC_TYPE_OSTREE)
      {
        *out_update = NULL;
        return TRUE; /* NB: early return */
      }

    /* just skip over "ostree://" so we can talk with libostree without thinking about it */
    refspec = refspec_data;
  }

  /* let's start with the ostree side of things */

  g_autofree char *new_checksum = NULL;
  if (!ostree_repo_resolve_rev_ext (repo, refspec, TRUE, 0, &new_checksum, error))
    return FALSE;

  const char *current_checksum = ostree_deployment_get_csum (deployment);
  gboolean is_layered;
  g_autofree char *current_checksum_owned = NULL;
  if (!rpmostree_deployment_get_layered_info (repo, deployment, &is_layered,
                                              &current_checksum_owned, NULL, NULL, NULL,
                                              error))
    return FALSE;
  if (is_layered)
    current_checksum = current_checksum_owned;

  /* Graciously handle rev no longer in repo; e.g. mucking around with rebase/rollback; we
   * still want to do the rpm-md phase. In that case, just use the current csum. */
  gboolean is_new_checksum = FALSE;
  if (!new_checksum)
    new_checksum = g_strdup (current_checksum);
  else
    is_new_checksum = !g_str_equal (new_checksum, current_checksum);

  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, new_checksum, &commit, NULL, error))
    return FALSE;

  g_auto(GVariantDict) dict;
  g_variant_dict_init (&dict, NULL);

  /* first get all the traditional/backcompat stuff */
  if (!add_all_commit_details_to_vardict (deployment, repo, refspec,
                                          new_checksum, commit, &dict, error))
    return FALSE;

  /* This may seem trivial, but it's important to keep the final variant as self-contained
   * and "diff-based" as possible, since it'll be available as a D-Bus property. This makes
   * it easier to consume for UIs like GNOME Software and Cockpit. */
  g_variant_dict_insert (&dict, "ref-has-new-commit", "b", is_new_checksum);

  g_auto(RpmDiff) rpm_diff = {0, };
  rpm_diff_init (&rpm_diff);

  /* we'll need this later for advisories, so just keep it around */
  g_autoptr(GPtrArray) ostree_modified_new = NULL;
  if (is_new_checksum)
    {
      g_autoptr(GPtrArray) removed = NULL;
      g_autoptr(GPtrArray) added = NULL;
      g_autoptr(GPtrArray) modified_old = NULL;

      /* Note we allow_noent here; we'll just skip over the rpm diff if there's no data */
      RpmOstreeDbDiffExtFlags flags = RPM_OSTREE_DB_DIFF_EXT_ALLOW_NOENT;
      if (!rpm_ostree_db_diff_ext (repo, current_checksum, new_checksum, flags, &removed,
                                   &added, &modified_old, &ostree_modified_new, NULL, error))
        return FALSE;

      /* check if allow_noent kicked in */
      if (removed)
        rpm_diff_add_base_db_diff (&rpm_diff, removed, added,
                                   modified_old, ostree_modified_new);
    }

  /* now we look at the rpm-md side */

  /* first we try to set up a sack (NULL --> no cache available) */
  g_autoptr(DnfSack) sack = NULL;
  if (!get_cached_rpmmd_sack (sysroot, repo, deployment, &sack, error))
    return FALSE;

  g_autoptr(GPtrArray) rpmmd_modified_new = NULL;

  GHashTable *layered_pkgs = rpmostree_origin_get_packages (origin);
  /* check that it's actually layered (i.e. the requests are not all just dormant) */
  if (sack && is_layered && g_hash_table_size (layered_pkgs) > 0)
    {
      if (!rpmmd_diff (sysroot, repo, deployment, current_checksum, sack, &rpm_diff,
                       &rpmmd_modified_new, error))
        return FALSE;
    }

  /* don't bother inserting if there's nothing new */
  if (!rpm_diff_is_empty (&rpm_diff))
    g_variant_dict_insert (&dict, "rpm-diff", "@a{sv}", rpm_diff_variant_new (&rpm_diff));

  /* but if there are no updates, then just ditch the whole thing and return NULL */
  if (is_new_checksum || rpmmd_modified_new)
    *out_update = g_variant_ref_sink (g_variant_dict_end (&dict));
  else
    *out_update = NULL;

  return TRUE;
}
