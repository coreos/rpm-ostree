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

int
rpmostree_builtin_deploy (int            argc,
                          char         **argv,
                          RpmOstreeCommandInvocation *invocation,
                          GCancellable  *cancellable,
                          GError       **error)
{
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autofree char *transaction_address = NULL;
  const char * const packages[] = { NULL };
  const char *revision;
  _cleanup_peer_ GPid peer_pid = 0;
  const char *const *install_pkgs = NULL;
  const char *const *uninstall_pkgs = NULL;

  context = g_option_context_new ("REVISION");

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       &install_pkgs,
                                       &uninstall_pkgs,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       error))
    return EXIT_FAILURE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "REVISION must be specified", error);
      return EXIT_FAILURE;
    }

  if (opt_preview && (install_pkgs != NULL || uninstall_pkgs != NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --preview and --install/--uninstall");
      return EXIT_FAILURE;
    }

  revision = argv[1];

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return EXIT_FAILURE;

  g_autoptr(GVariant) previous_deployment = rpmostree_os_dup_default_deployment (os_proxy);

  if (opt_preview)
    {
      if (!rpmostree_os_call_download_deploy_rpm_diff_sync (os_proxy,
                                                            revision,
                                                            packages,
                                                            &transaction_address,
                                                            cancellable,
                                                            error))
        return EXIT_FAILURE;
    }
  else
    {
      g_autoptr(GVariant) options =
        rpmostree_get_options_variant (opt_reboot,
                                       TRUE,   /* allow-downgrade */
                                       FALSE,  /* skip-purge */
                                       FALSE,  /* no-pull-base */
                                       FALSE,  /* dry-run */
                                       FALSE); /* no-overrides */


      /* Use newer D-Bus API only if we have to so we maintain coverage. */
      if (install_pkgs || uninstall_pkgs)
        {
          if (!rpmostree_update_deployment (os_proxy,
                                            NULL, /* refspec */
                                            revision,
                                            install_pkgs,
                                            uninstall_pkgs,
                                            NULL, /* override replace */
                                            NULL, /* override remove */
                                            NULL, /* override reset */
                                            options,
                                            &transaction_address,
                                            cancellable,
                                            error))
            return EXIT_FAILURE;
        }
      else
        {
          if (!rpmostree_os_call_deploy_sync (os_proxy,
                                              revision,
                                              options,
                                              NULL,
                                              &transaction_address,
                                              NULL,
                                              cancellable,
                                              error))
            return EXIT_FAILURE;
        }
    }

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return EXIT_FAILURE;

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
        return EXIT_FAILURE;

      if (g_variant_n_children (result) == 0)
        return RPM_OSTREE_EXIT_UNCHANGED;

      rpmostree_print_package_diffs (result);
    }
  else if (!opt_reboot)
    {
      if (!rpmostree_has_new_default_deployment (os_proxy, previous_deployment))
        return RPM_OSTREE_EXIT_UNCHANGED;

      /* do diff without dbus: https://github.com/projectatomic/rpm-ostree/pull/116 */
      const char *sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);
      if (!rpmostree_print_treepkg_diff_from_sysroot_path (sysroot_path,
                                                           cancellable,
                                                           error))
        return EXIT_FAILURE;

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  return EXIT_SUCCESS;
}
