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
                                   const gchar *csum)
{
  GError *error = NULL;
  GVariant *ret = NULL;

  g_autofree gchar *remote = NULL;
  glnx_unref_object OstreeGpgVerifyResult *result = NULL;

  guint n_sigs, i;
  gboolean gpg_verify;
  GVariantBuilder builder;

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

  if (!gpg_verify)
    goto out;


  result = ostree_repo_verify_commit_ext (repo, csum, NULL, NULL,
                                          NULL, &error);
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

char *
rpmostreed_deployment_get_refspec (OstreeDeployment *deployment)
{
  char *origin_refspec = NULL;
  rpmostreed_deployment_get_refspec_packages (deployment, &origin_refspec, NULL);
  return origin_refspec;
}

void
rpmostreed_deployment_get_refspec_packages (OstreeDeployment *deployment,
					    char            **out_refspec,
					    char           ***out_packages)
{
  GKeyFile *origin = NULL; /* owned by deployment */
  gsize len;

  g_return_if_fail (out_refspec != NULL);

  origin = ostree_deployment_get_origin (deployment);

  if (!origin)
    {
      *out_refspec = NULL;
      *out_packages = NULL;
      return;
    }

  *out_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
  if (!*out_refspec)
    *out_refspec = g_key_file_get_string (origin, "origin", "baserefspec", NULL);

  if (out_packages)
    *out_packages = g_key_file_get_string_list (origin, "packages", "requested", &len, NULL);
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
                            OstreeRepo *repo,
                            const gchar *csum)
{
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) metadata = NULL;
  GError *error = NULL;

  g_autofree gchar *version_commit = NULL;
  guint64 timestamp = 0;

  if (ostree_repo_load_variant (repo,
                                OSTREE_OBJECT_TYPE_COMMIT,
                                csum,
                                &commit,
                                &error))
    {

      timestamp = ostree_commit_get_timestamp (commit);
      metadata = g_variant_get_child_value (commit, 0);
      if (metadata != NULL)
          g_variant_lookup (metadata, "version", "s", &version_commit);
    }
  else
    {
      g_warning ("Error loading commit %s", error->message);
    }

  if (version_commit != NULL)
    g_variant_dict_insert (dict, "version", "s", version_commit);
  if (timestamp > 0)
    g_variant_dict_insert (dict, "timestamp", "t", timestamp);

  g_clear_error (&error);
}

GVariant *
rpmostreed_deployment_generate_variant (OstreeDeployment *deployment,
                                        OstreeRepo *repo)
{
  g_autoptr(GVariant) commit = NULL;

  g_autofree gchar *origin_refspec = NULL;
  g_auto(GStrv) origin_packages = NULL;
  g_autofree gchar *id = NULL;

  GVariant *sigs = NULL; /* floating variant */

  GVariantDict dict;

  const gchar *osname = ostree_deployment_get_osname (deployment);
  const gchar *csum = ostree_deployment_get_csum (deployment);
  gint serial = ostree_deployment_get_deployserial (deployment);
  id = rpmostreed_deployment_generate_id (deployment);

  rpmostreed_deployment_get_refspec_packages (deployment, &origin_refspec, &origin_packages);
  if (origin_refspec)
    sigs = rpmostreed_deployment_gpg_results (repo, origin_refspec, csum);

  g_variant_dict_init (&dict, NULL);

  g_variant_dict_insert (&dict, "id", "s", id);
  if (osname != NULL)
    g_variant_dict_insert (&dict, "osname", "s", osname);
  g_variant_dict_insert (&dict, "serial", "i", serial);
  g_variant_dict_insert (&dict, "checksum", "s", csum);

  variant_add_commit_details (&dict, repo, csum);
  if (origin_refspec != NULL)
    g_variant_dict_insert (&dict, "origin", "s", origin_refspec);
  if (origin_packages != NULL)
    g_variant_dict_insert (&dict, "packages", "^as", origin_packages);
  if (sigs != NULL)
    g_variant_dict_insert_value (&dict, "signatures", sigs);

  return g_variant_dict_end (&dict);
}

GVariant *
rpmostreed_commit_generate_cached_details_variant (OstreeDeployment *deployment,
                                                   OstreeRepo *repo,
                                                   const gchar *refspec)
{
  g_autofree gchar *origin_refspec = NULL;
  g_autofree gchar *head = NULL;
  const gchar *osname;

  GVariant *sigs = NULL; /* floating variant */
  GVariant *ret = NULL; /* floating variant */
  GError *error = NULL;

  GVariantDict dict;

  osname = ostree_deployment_get_osname (deployment);

  if (refspec)
    origin_refspec = g_strdup (refspec);
  else
    origin_refspec = rpmostreed_deployment_get_refspec (deployment);

  if (!origin_refspec)
    goto out;

  if (!ostree_repo_resolve_rev (repo, origin_refspec,
                                FALSE, &head, &error))
    {
      g_warning ("Error loading resolving revision: %s", error->message);
      goto out;
    }

  sigs = rpmostreed_deployment_gpg_results (repo, origin_refspec, head);

  g_variant_dict_init (&dict, NULL);
  if (osname != NULL)
    g_variant_dict_insert (&dict, "osname", "s", osname);
  g_variant_dict_insert (&dict, "checksum", "s", head);
  variant_add_commit_details (&dict, repo, head);
  g_variant_dict_insert (&dict, "origin", "s", origin_refspec);
  if (sigs != NULL)
    g_variant_dict_insert_value (&dict, "signatures", sigs);
  ret = g_variant_dict_end (&dict);
out:
  g_clear_error (&error);
  return ret;
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
