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

#include <string.h>
#include <glib-unix.h>

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"

#include "libgsystem.h"

static char *opt_sysroot = "/";
static gboolean opt_reboot;
static gboolean opt_force_peer;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after rollback is prepared", NULL },
  { "peer", 0, 0, G_OPTION_ARG_NONE, &opt_force_peer, "Force a peer to peer connection instead of using the system message bus", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_rollback (int             argc,
                            char          **argv,
                            GCancellable   *cancellable,
                            GError        **error)
{
  GOptionContext *context = g_option_context_new ("- Revert to the previously booted tree");
  gs_unref_object GDBusConnection *connection = NULL;
  gs_unref_object RPMOSTreeManager *manager = NULL;
  gs_unref_object RPMOSTreeDeployment *booted_deployment = NULL;
  gs_unref_object RPMOSTreeDeployment *new_default_deployment = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_unref_variant GVariant *variant_args = NULL;

  gs_free gchar *booted_deployment_path = NULL;
  gs_free gchar *new_deployment_path = NULL;
  gs_free gchar *new_csum = NULL;
  gs_strfreev gchar **deployment_paths = NULL;

  guint n;
  guint booted_index;
  gboolean ret = FALSE;
  gboolean is_peer = FALSE;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  if (!rpmostree_load_connection_and_manager (opt_sysroot,
                                              opt_force_peer,
                                              cancellable,
                                              &connection,
                                              &manager,
                                              &is_peer,
                                              error))
    goto out;

  // populate deployment information
  variant_args = g_variant_ref_sink (g_variant_new ("a{sv}", NULL));
  booted_deployment_path = rpmostree_manager_dup_booted_deployment (manager);
  if (rpmostree_is_valid_object_path (booted_deployment_path))
    {
      booted_deployment = rpmostree_deployment_proxy_new_sync (connection,
                                                      G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                      is_peer ? NULL : BUS_NAME,
                                                      booted_deployment_path,
                                                      cancellable,
                                                      error);
    }
  else
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system");
    }

  if (booted_deployment == NULL)
    goto out;

  if (!rpmostree_manager_call_get_deployments_sync (manager,
                                                    variant_args,
                                                    &deployment_paths,
                                                    cancellable,
                                                    error))
    goto out;

  n = g_strv_length (deployment_paths);
  if (n < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Found %u deployments, at least 2 required for rollback",
                   n);
      goto out;
    }

  for (booted_index = 0; booted_index < n; booted_index++)
    {
      if (g_strcmp0 (booted_deployment_path, deployment_paths[booted_index]) == 0)
        break;
    }
  g_assert (booted_index < n);

  if (booted_index != 0)
    {
      /* There is an earlier deployment, let's assume we want to just
       * insert the current one in front.
       */

       /*
       What this does is, if we're NOT in the default boot index, it plans to prepend
       our current index (1, since we can't have more than two trees) so that it becomes index 0
       (default) and the current default becomes index 1
       */
      new_deployment_path = g_strdup (booted_deployment_path);
    }
  else
    {
      /* We're booted into the first, let's roll back to the previous */
      new_deployment_path = g_strdup (deployment_paths[1]);
    }

  new_default_deployment = rpmostree_deployment_proxy_new_sync (connection,
                                                  G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                  is_peer ? NULL : BUS_NAME,
                                                  new_deployment_path,
                                                  cancellable,
                                                  error);
  if (!new_default_deployment)
    goto out;


  new_csum = rpmostree_deployment_dup_checksum (new_default_deployment);
  g_print ("Moving '%s.%d' to be first deployment\n", new_csum,
           rpmostree_deployment_get_serial (new_default_deployment));

  if (!rpmostree_deployment_deploy_sync (manager,
                                         new_default_deployment,
                                         cancellable,
                                         error))
    goto out;

  if (opt_reboot)
    {
      gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                     cancellable, error,
                                     "systemctl", "reboot", NULL);
    }
  else
    {
      gs_unref_variant GVariant *out_difference;
      if (!rpmostree_deployment_call_get_rpm_diff_sync (new_default_deployment,
                                                        &out_difference,
                                                        cancellable,
                                                        error))
        goto out;

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  ret = TRUE;

out:
  return ret;
}
