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

#include "deployment-utils.h"

#include <libglnx.h>

char *
deployment_generate_id (OstreeDeployment *deployment)
{
  const char *osname;
  guint hash;

  g_return_val_if_fail (OSTREE_IS_DEPLOYMENT (deployment), NULL);

  osname = ostree_deployment_get_osname (deployment);
  hash = ostree_deployment_hash (deployment);

  return g_strdup_printf ("%s_%u", osname, hash);
}

static GVariant *
deployment_gpg_results (OstreeRepo *repo,
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

  if (!ostree_repo_remote_get_gpg_verify (repo, remote, &gpg_verify, &error))
    goto out;

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
deployment_get_refspec (OstreeDeployment *deployment)
{
  GKeyFile *origin = NULL; // owned by deployment
  char *origin_refspec = NULL;
  origin = ostree_deployment_get_origin (deployment);

  if (!origin)
    goto out;

  origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);

out:
  return origin_refspec;
}

GVariant *
deployment_generate_blank_variant (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
  g_variant_builder_add (&builder, "s", "");
  g_variant_builder_add (&builder, "s", "");
  g_variant_builder_add (&builder, "i", -1);
  g_variant_builder_add (&builder, "s", "");
  g_variant_builder_add (&builder, "s", "");
  g_variant_builder_add (&builder, "t", 0);
  g_variant_builder_add (&builder, "s", "");
  g_variant_builder_add_value (&builder, g_variant_new("av", NULL));
  return g_variant_builder_end (&builder);
}

GVariant *
deployment_generate_variant (OstreeDeployment *deployment,
                             OstreeRepo *repo)
{
  g_autoptr (GVariant) commit = NULL;

  g_autofree gchar *origin_refspec = NULL;
  g_autofree gchar *version_commit = NULL;
  g_autofree gchar *id = NULL;

  GVariant *sigs = NULL; // floating variant
  GError *error = NULL;

  GVariantBuilder builder;
  guint64 timestamp = 0;

  const gchar *osname = ostree_deployment_get_osname (deployment);
  const gchar *csum = ostree_deployment_get_csum (deployment);
  gint serial = ostree_deployment_get_deployserial (deployment);
  id = deployment_generate_id (deployment);

  if (ostree_repo_load_variant (repo,
                                OSTREE_OBJECT_TYPE_COMMIT,
                                csum,
                                &commit,
                                &error))
    {
      g_autoptr (GVariant) metadata = NULL;
      timestamp = ostree_commit_get_timestamp (commit);
      metadata = g_variant_get_child_value (commit, 0);
      if (metadata != NULL)
          g_variant_lookup (metadata, "version", "s", &version_commit);
    }
  else
    {
      g_warning ("Error loading commit %s", error->message);
    }
  g_clear_error (&error);

  origin_refspec = deployment_get_refspec (deployment);
  if (origin_refspec)
    sigs = deployment_gpg_results (repo, origin_refspec, csum);

  if (!sigs)
    sigs = g_variant_new ("av", NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
  g_variant_builder_add (&builder, "s", id);
  g_variant_builder_add (&builder, "s", osname ? osname : "");
  g_variant_builder_add (&builder, "i", serial);
  g_variant_builder_add (&builder, "s", csum);
  g_variant_builder_add (&builder, "s", version_commit ? version_commit : "");
  g_variant_builder_add (&builder, "t", timestamp ? timestamp : 0);
  g_variant_builder_add (&builder, "s", origin_refspec ? origin_refspec : "none");
  g_variant_builder_add_value (&builder, sigs);

  return g_variant_builder_end (&builder);
}
