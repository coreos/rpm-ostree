/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Anne LoVerso <anne.loverso@students.olin.edu>
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

#include <string.h>
#include <glib-unix.h>

#include "rpmostree-builtins.h"
#include "rpmostree-treepkgdiff.h"

#include "libgsystem.h"

gboolean
rpmostree_builtin_status (int             argc,
                              char          **argv,
                              GCancellable   *cancellable,
                              GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;  // list of all depoyments
  OstreeDeployment *booted_deployment = NULL;   // current booted deployment
  GOptionContext *context = g_option_context_new ("- Get the version of the booted system");
  char *default_boot_message = NULL;    // is this deployment default? 
  char *rollback_instructions = NULL;       // how to use rollback to change default; depending on state
  char *version_message = NULL;        // is this the most recent version?
  OstreeDeployment *most_recent = NULL; // most recent deployment
  guint i;
  guint j;

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  sysroot = ostree_sysroot_new_default ();
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  if (booted_deployment == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system");
      goto out;
    }

  deployments = ostree_sysroot_get_deployments (sysroot);
  
  g_assert (booted_deployment != NULL);

  if (deployments->len < 2)
    {
      g_print ("Note: Fewer than two versions found\n");
      default_boot_message = rollback_instructions = version_message = "";
    }
  else
    {
      // determine most recent version
      for (i=0; i<deployments->len; i++)
        {
          if (most_recent == NULL)
            most_recent = deployments->pdata[i];
          else
            {
              gs_unref_variant GVariant *version = NULL;
              gs_unref_variant GVariant *recent_version = NULL;
              gs_unref_object OstreeRepo *repo = NULL;
              const char *csum = ostree_deployment_get_csum (deployments->pdata[i]);
              const char *recent_csum = ostree_deployment_get_csum (most_recent);

              if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
                goto out;

              if (!ostree_repo_load_variant (repo,
                                           OSTREE_OBJECT_TYPE_COMMIT,
                                           csum,
                                           &version,
                                           error))
                goto out;

              if (!ostree_repo_load_variant (repo,
                                           OSTREE_OBJECT_TYPE_COMMIT,
                                           recent_csum,
                                           &recent_version,
                                           error))
                goto out;
              
              // the newer (most recent) version will have a larger timestamp
              // i.e. if a > b then a is newer than b
              if (ostree_commit_get_timestamp (version) > ostree_commit_get_timestamp (recent_version))
                  most_recent = deployments->pdata[i];
            }
        }

      if (deployments->pdata[0] == booted_deployment) // if current deployment is default
        {
          default_boot_message = "The current booted deployment is the default\n";
          rollback_instructions = "Use command 'rpm-ostree rollback' to switch defaults\n";
        }
      else // current boot not default
        {
          default_boot_message = "The current booted deployment is not the default\n";
          rollback_instructions = "Use command 'rpm-ostree rollback' to make it default\n";
        }
      version_message = (booted_deployment == most_recent) 
                      ? "\nThe current booted deployment is the most recent upgrade\n"
                      : "\nThe current booted deployment is not the most recent upgrade\n";
    }

  for (j=0; j<deployments->len; j++)
    {
      OstreeDeployment *deployment = deployments->pdata[j];
      GKeyFile *origin;

      g_print ("%c %c %s %s.%d\n",
               deployment == most_recent ? 'r' : ' ',
               deployment == booted_deployment ? '*' : ' ',
               ostree_deployment_get_osname (deployment),
               ostree_deployment_get_csum (deployment),
               ostree_deployment_get_deployserial (deployment));

      origin = ostree_deployment_get_origin (deployment);
      if (!origin)
        g_print ("      origin: none\n");
      else
        {
          gs_free char *origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
          if (!origin_refspec)
            g_print ("      origin: <unknown origin type>\n");
          else
            g_print ("      origin refspec: %s\n", origin_refspec);
        }
    } 

    g_print ("%s", version_message);
    g_print ("%s", default_boot_message);
    g_print ("%s", rollback_instructions);

  ret = TRUE;
  out:
  	return ret;
}
