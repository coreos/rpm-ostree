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
#include "rpmostree-util.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"

#include "libgsystem.h"

static char *opt_sysroot = "/";
static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_skip_purge;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after rebase is finished", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_skip_purge, "Keep previous refspec after rebase", NULL },
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
  g_variant_builder_add (&builder, "{sv}", "skip-purge",
                         g_variant_new("b", opt_skip_purge));
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}


gboolean
rpmostree_builtin_rebase (int             argc,
                          char          **argv,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("REFSPEC - Switch to a different tree");
  gs_unref_object GDBusConnection *connection = NULL;
  gs_unref_object RPMOSTreeManager *manager = NULL;
  gs_unref_object RPMOSTreeRefSpec *refspec = NULL;
  gs_free gchar *refspec_path = NULL;
  gs_unref_variant GVariant *variant_args = NULL;
  gs_unref_variant GVariant *variant_path = NULL;

  const char *new_provided_refspec;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  if (!opt_sysroot)
    opt_sysroot = "/";

  if (argc < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "REFSPEC must be specified");
      goto out;
    }

  new_provided_refspec = argv[1];

  connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, cancellable, error);
  if (!connection)
    goto out;

  // Get manager
  manager = rpmostree_manager_proxy_new_sync (connection,
                                              G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                              BUS_NAME,
                                              "/org/projectatomic/rpmostree1/Manager",
                                              cancellable,
                                              error);
  if (!manager)
    goto out;

  variant_args = get_args_variant ();
  if (!rpmostree_manager_call_add_ref_spec_sync (manager, variant_args,
                                                 new_provided_refspec,
                                                 &refspec_path, cancellable,
                                                 error))
    goto out;

  refspec = rpmostree_ref_spec_proxy_new_sync (connection,
                                               G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                               BUS_NAME,
                                               refspec_path,
                                               cancellable,
                                               error);
  if (refspec == NULL)
      goto out;

  if (!rpmostree_refspec_update_sync (manager, refspec, "Deploy",
                                      g_variant_new ("(@a{sv})", variant_args),
                                      cancellable, error))
    goto out;

  if (!opt_reboot)
    {
      gs_unref_variant GVariant *out_difference;
      if (!rpmostree_ref_spec_call_get_rpm_diff_sync (refspec,
                                                      &out_difference,
                                                      cancellable,
                                                      error))
        goto out;

      g_print ("diff will be here: %s\n", g_variant_print (out_difference, TRUE));
      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }
  else
    {
      gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                     cancellable, error,
                                     "systemctl", "reboot", NULL);
    }

  ret = TRUE;

out:
  return ret;
}
