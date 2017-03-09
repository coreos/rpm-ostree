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

#include "rpmostreed-deployment-utils.h"
#include "rpmostree-origin.h"
#include "rpmostree-util.h"

#include <libglnx.h>

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
  g_autoptr(GPtrArray) deployments = NULL;
  guint i;

  OstreeDeployment *deployment = NULL;

  deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployments == NULL)
    goto out;

  for (i=0; i<deployments->len; i++)
    {
      g_autofree gchar *id = rpmostreed_deployment_generate_id (deployments->pdata[i]);
      if (g_strcmp0 (deploy_id, id) == 0) {
        deployment = g_object_ref (deployments->pdata[i]);
      }
    }

out:
  return deployment;
}

static GVariant *
rpmostreed_deployment_gpg_results (OstreeRepo *repo,
                                   const gchar *origin_refspec,
                                   const gchar *csum,
				   gboolean *out_enabled)
{
  GError *error = NULL;
  GVariant *ret = NULL;

  g_autofree gchar *remote = NULL;
  glnx_unref_object OstreeGpgVerifyResult *result = NULL;

  guint n_sigs, i;
  gboolean gpg_verify;
  g_auto(GVariantBuilder) builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));

  if (!ostree_parse_refspec (origin_refspec, &remote, NULL, &error))
    goto out;

  if (remote)
    {
      if (!ostree_repo_remote_get_gpg_verify (repo, remote, &gpg_verify, &error))
	goto out;
    }
  else
    {
      gpg_verify = FALSE;
    }

  *out_enabled = gpg_verify;
  if (!gpg_verify)
    goto out;

  result = ostree_repo_verify_commit_for_remote (repo, csum, remote, NULL, &error);
  if (!result)
      goto out;

  n_sigs = ostree_gpg_verify_result_count_all (result);
  if (n_sigs < 1)
    goto out;

  for (i = 0; i < n_sigs; i++)
    {
      g_variant_builder_add (&builder, "v",
                             ostree_gpg_verify_result_get_all (result, i));
    }

  ret = g_variant_builder_end (&builder);

out:

  /* NOT_FOUND just means the commit is not signed. */
  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_warning ("error loading gpg verify result %s", error->message);

  g_clear_error (&error);
  return ret;
}

GVariant *
rpmostreed_deployment_generate_blank_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);

  return g_variant_dict_end (&dict);
}

static void
variant_add_commit_details (GVariantDict *dict,
                            const char *prefix,
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

GVariant *
rpmostreed_deployment_generate_variant (OstreeDeployment *deployment,
                                        const char *booted_id,
                                        OstreeRepo *repo,
                                        GError **error)
{
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(RpmOstreeOrigin) origin = NULL;
  g_autofree gchar *id = NULL;
  g_autofree char *base_checksum = NULL;

  GVariant *sigs = NULL; /* floating variant */

  GVariantDict dict;

  const char *refspec;
  g_autofree char *pending_base_commitrev = NULL;
  const gchar *osname = ostree_deployment_get_osname (deployment);
  const gchar *csum = ostree_deployment_get_csum (deployment);
  gint serial = ostree_deployment_get_deployserial (deployment);
  gboolean gpg_enabled = FALSE;
  gboolean is_layered = FALSE;
  g_auto(GStrv) layered_pkgs = NULL;

  if (!ostree_repo_load_variant (repo,
				 OSTREE_OBJECT_TYPE_COMMIT,
				 csum,
				 &commit,
				 error))
    return NULL;
  
  id = rpmostreed_deployment_generate_id (deployment);

  origin = rpmostree_origin_parse_deployment (deployment, error);
  if (!origin)
    return NULL;

  refspec = rpmostree_origin_get_refspec (origin);

  g_variant_dict_init (&dict, NULL);

  g_variant_dict_insert (&dict, "id", "s", id);
  if (osname != NULL)
    g_variant_dict_insert (&dict, "osname", "s", osname);
  g_variant_dict_insert (&dict, "serial", "i", serial);
  g_variant_dict_insert (&dict, "checksum", "s", csum);

  if (!rpmostree_deployment_get_layered_info (repo, deployment, &is_layered,
                                              &base_checksum, &layered_pkgs,
                                              error))
    return NULL;

  if (is_layered)
    g_variant_dict_insert (&dict, "base-checksum", "s", base_checksum);
  else
    base_checksum = g_strdup (csum);

  sigs = rpmostreed_deployment_gpg_results (repo, refspec, base_checksum, &gpg_enabled);
  variant_add_commit_details (&dict, NULL, commit);

  if (!ostree_repo_resolve_rev (repo, refspec, TRUE,
                                &pending_base_commitrev, error))
    return NULL;

  if (pending_base_commitrev && strcmp (pending_base_commitrev, base_checksum) != 0)
    {
      g_autoptr(GVariant) pending_base_commit = NULL;

      if (!ostree_repo_load_variant (repo,
                                     OSTREE_OBJECT_TYPE_COMMIT,
                                     pending_base_commitrev,
                                     &pending_base_commit,
                                     error))
        return NULL;

      g_variant_dict_insert (&dict, "pending-base-checksum", "s", pending_base_commitrev);
      variant_add_commit_details (&dict, "pending-base-", pending_base_commit);
    }

  g_variant_dict_insert (&dict, "origin", "s", refspec);

  g_autofree char **requested_pkgs =
    (char**)g_hash_table_get_keys_as_array (rpmostree_origin_get_packages (origin), NULL);
  g_variant_dict_insert (&dict, "requested-packages", "^as", requested_pkgs);

  if (is_layered && g_strv_length (layered_pkgs) > 0)
    g_variant_dict_insert (&dict, "packages", "^as", layered_pkgs);
  else
    {
      const char *const p[] = { NULL };
      g_variant_dict_insert (&dict, "packages", "^as", p);
    }

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

GVariant *
rpmostreed_commit_generate_cached_details_variant (OstreeDeployment *deployment,
                                                   OstreeRepo *repo,
                                                   const gchar *refspec,
						   GError **error)
{
  g_autoptr(GVariant) commit = NULL;
  g_autofree gchar *origin_refspec = NULL;
  g_autofree gchar *head = NULL;
  gboolean gpg_enabled;
  const gchar *osname;
  GVariant *sigs = NULL; /* floating variant */
  GVariantDict dict;

  osname = ostree_deployment_get_osname (deployment);

  if (refspec)
    origin_refspec = g_strdup (refspec);
  else
    {
      g_autoptr(RpmOstreeOrigin) origin = NULL;

      origin = rpmostree_origin_parse_deployment (deployment, error);
      if (!origin)
        return NULL;
      origin_refspec = g_strdup (rpmostree_origin_get_refspec (origin));
    }

  g_assert (origin_refspec);

  /* allow_noent=TRUE since the ref may have been deleted for a
   * rebase.
   */
  if (!ostree_repo_resolve_rev (repo, origin_refspec,
                                TRUE, &head, error))
    return NULL;

  if (head == NULL)
    head = g_strdup (ostree_deployment_get_csum (deployment));

  if (!ostree_repo_load_variant (repo,
				 OSTREE_OBJECT_TYPE_COMMIT,
				 head,
				 &commit,
				 error))
    return NULL;

  sigs = rpmostreed_deployment_gpg_results (repo, origin_refspec, head, &gpg_enabled);

  g_variant_dict_init (&dict, NULL);
  if (osname != NULL)
    g_variant_dict_insert (&dict, "osname", "s", osname);
  g_variant_dict_insert (&dict, "checksum", "s", head);
  variant_add_commit_details (&dict, NULL, commit);
  g_variant_dict_insert (&dict, "origin", "s", origin_refspec);
  if (sigs != NULL)
    g_variant_dict_insert_value (&dict, "signatures", sigs);
  g_variant_dict_insert (&dict, "gpg-enabled", "b", gpg_enabled);
  return g_variant_dict_end (&dict);
}

gint
rpmostreed_rollback_deployment_index (const gchar *name,
                                      OstreeSysroot *ot_sysroot,
                                      GError **error)
{
  g_autoptr(GPtrArray) deployments = NULL;
  glnx_unref_object OstreeDeployment *merge_deployment = NULL;

  gint index_to_prepend = -1;
  gint merge_index = -1;
  gint previous_index = -1;
  guint i;

  merge_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
  if (merge_deployment == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No deployments found for os %s", name);
      goto out;
    }

  deployments = ostree_sysroot_get_deployments (ot_sysroot);
  if (deployments->len < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Found %u deployments, at least 2 required for rollback",
                   deployments->len);
      goto out;
    }

  g_assert (merge_deployment != NULL);
  for (i = 0; i < deployments->len; i++)
    {
      if (deployments->pdata[i] == merge_deployment)
        merge_index = i;

      if (g_strcmp0 (ostree_deployment_get_osname (deployments->pdata[i]), name) == 0 &&
              deployments->pdata[i] != merge_deployment &&
              previous_index < 0)
        {
            previous_index = i;
        }
    }

  g_assert (merge_index < deployments->len);
  g_assert (deployments->pdata[merge_index] == merge_deployment);

  /* If merge deployment is not booted assume we are using it. */
  if (merge_index == 0 && previous_index > 0)
      index_to_prepend = previous_index;
  else
      index_to_prepend = merge_index;

out:
  return index_to_prepend;
}
