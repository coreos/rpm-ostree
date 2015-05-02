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
#include <gio/gio.h>

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-dbus-helpers.h"

#include "libgsystem.h"

static char *opt_sysroot = "/";
static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_allow_downgrade;
static gboolean opt_check_diff;
static gboolean opt_force_peer;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after an upgrade is prepared", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { "check-diff", 0, 0, G_OPTION_ARG_NONE, &opt_check_diff, "Check for upgrades and print package diff only", NULL },
  { "peer", 0, 0, G_OPTION_ARG_NONE, &opt_force_peer, "Force a peer to peer connection instead of using the system message bus", NULL },
  { NULL }
};


static GVariant *
get_args_variant (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  if (opt_osname)
    g_variant_builder_add (&builder, "{sv}", "os",
                           g_variant_new("s", opt_osname));
  g_variant_builder_add (&builder, "{sv}", "allow-downgrade",
                         g_variant_new("b", opt_allow_downgrade));
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}


static void
transfer_changed_callback (GObject *object,
                           GParamSpec *pspec,
                           gpointer user_data)
{
  gs_free gchar *value = NULL;
  gchar **out_value = user_data;
  g_object_get(object, pspec->name, value, NULL);

  if (value && out_value)
    gs_transfer_out_value (out_value, &value);
}


gboolean
rpmostree_builtin_upgrade (int             argc,
                           char          **argv,
                           GCancellable   *cancellable,
                           GError        **error)
{
  gboolean ret = FALSE;
  gboolean is_peer = FALSE;

  GOptionContext *context = g_option_context_new ("- Perform a system upgrade");
  gs_unref_object GDBusConnection *connection = NULL;
  gs_unref_object RPMOSTreeManager *manager = NULL;
  gs_unref_object RPMOSTreeRefSpec *refspec = NULL;
  gs_free gchar *refspec_path = NULL;
  gs_free gchar *remote = NULL;
  gs_free gchar *ref = NULL;
  gs_unref_variant GVariant *variant_args = NULL;
  gs_free gchar *new_deployment_path = NULL;
  gs_free gchar *new_head = NULL;
  gboolean changed = FALSE;

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

  variant_args = get_args_variant ();
  if (!rpmostree_manager_call_get_upgrade_ref_spec_sync (manager,
                                                         variant_args,
                                                         &refspec_path,
                                                         cancellable,
                                                         error))
    goto out;

  if (rpmostree_is_valid_object_path (refspec_path))
    {
      refspec = rpmostree_ref_spec_proxy_new_sync (connection,
                                                   G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                   is_peer ? NULL : BUS_NAME,
                                                   refspec_path,
                                                   cancellable,
                                                   error);
    }
  else
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not find remote to upgrade");

    }

  if (refspec == NULL)
      goto out;

  g_signal_connect (manager, "notify::DefaultDeployment",
                    G_CALLBACK (transfer_changed_callback),
                    &new_deployment_path);
  g_signal_connect (manager, "notify::Head",
                    G_CALLBACK (transfer_changed_callback),
                    &new_head);

  remote = rpmostree_ref_spec_dup_remote_name (refspec);
  ref = rpmostree_ref_spec_dup_ref (refspec);
  g_print ("Updating from %s:%s\n", remote, ref);
  if (opt_check_diff)
    {
      if (!rpmostree_refspec_update_sync (manager, refspec, "PullRpmDb",
                                          NULL, cancellable, error))
        goto out;

      changed = new_deployment_path != NULL;
    }
  else
    {
      if (!rpmostree_refspec_update_sync (manager, refspec, "Deploy",
                                          g_variant_new ("(@a{sv})", variant_args),
                                          cancellable, error))
        goto out;

      changed = new_head != NULL;
    }

  if (!changed)
    goto out;

  if (opt_check_diff || !opt_reboot)
    {
      gs_unref_variant GVariant *out_difference;
      if (!rpmostree_ref_spec_call_get_rpm_diff_sync (refspec,
                                                      &out_difference,
                                                      cancellable,
                                                      error))
        goto out;

      g_print ("diff will be here: %s\n", g_variant_print (out_difference, TRUE));
    }

  if (!opt_check_diff)
    {
      if (opt_reboot)
        {
          gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                         cancellable, error,
                                         "systemctl", "reboot", NULL);
        }
      else
        {
          g_print ("Run \"systemctl reboot\" to start a reboot\n");
        }
    }

  ret = TRUE;

out:
  return ret;
}
