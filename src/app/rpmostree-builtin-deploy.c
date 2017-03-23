/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-dbus-helpers.h"

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_preview;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after upgrade is prepared", NULL },
  /* XXX As much as I dislike the inconsistency with "rpm-ostree upgrade",
   *     calling this option --check-diff doesn't really make sense here.
   *     A --preview option would work for both commands if we wanted to
   *     deprecate --check-diff. */
  { "preview", 0, 0, G_OPTION_ARG_NONE, &opt_preview, "Just preview package differences", NULL },
  { NULL }
};

static GVariant *
get_args_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);

  return g_variant_dict_end (&dict);
}

static void
default_deployment_changed_cb (GObject *object,
                               GParamSpec *pspec,
                               GVariant **value)
{
  g_object_get (object, pspec->name, value, NULL);
}

int
rpmostree_builtin_deploy (int            argc,
                          char         **argv,
                          RpmOstreeCommandInvocation *invocation,
                          GCancellable  *cancellable,
                          GError       **error)
{
  int exit_status = EXIT_FAILURE;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GVariant) default_deployment = NULL;
  g_autofree char *transaction_address = NULL;
  const char * const packages[] = { NULL };
  const char *revision;

  context = g_option_context_new ("REVISION - Deploy a specific commit");

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       &sysroot_proxy,
                                       error))
    goto out;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "REVISION must be specified", error);
      goto out;
    }

  revision = argv[1];

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    goto out;

  if (opt_preview)
    {
      if (!rpmostree_os_call_download_deploy_rpm_diff_sync (os_proxy,
                                                            revision,
                                                            packages,
                                                            &transaction_address,
                                                            cancellable,
                                                            error))
        goto out;
    }
  else
    {
      /* This will set the GVariant if the default deployment changes. */
      g_signal_connect (os_proxy, "notify::default-deployment",
                        G_CALLBACK (default_deployment_changed_cb),
                        &default_deployment);

      if (!rpmostree_os_call_deploy_sync (os_proxy,
                                          revision,
                                          get_args_variant (),
                                          NULL,
                                          &transaction_address,
                                          NULL,
                                          cancellable,
                                          error))
        goto out;
    }

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    goto out;

  if (opt_preview)
    {
      g_autoptr(GVariant) result = NULL;
      g_autoptr(GVariant) details = NULL;

      if (!rpmostree_os_call_get_cached_deploy_rpm_diff_sync (os_proxy,
                                                              revision,
                                                              packages,
                                                              &result,
                                                              &details,
                                                              cancellable,
                                                              error))
        goto out;

      if (g_variant_n_children (result) == 0)
        {
          exit_status = RPM_OSTREE_EXIT_UNCHANGED;
          goto out;
        }

      rpmostree_print_package_diffs (result);
    }
  else if (!opt_reboot)
    {
      const char *sysroot_path;

      if (default_deployment == NULL)
        {
          exit_status = RPM_OSTREE_EXIT_UNCHANGED;
          goto out;
        }

      sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);

      if (!rpmostree_print_treepkg_diff_from_sysroot_path (sysroot_path,
                                                           cancellable,
                                                           error))
        goto out;

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  exit_status = EXIT_SUCCESS;

out:
  /* Does nothing if using the message bus. */
  rpmostree_cleanup_peer ();

  return exit_status;
}
