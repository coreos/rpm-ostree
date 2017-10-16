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

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_allow_downgrade;
static gboolean opt_preview;
static gboolean opt_check;
static gboolean opt_upgrade_unchanged_exit_77;
static gboolean opt_cache_only;

/* "check-diff" is deprecated, replaced by "preview" */
static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after an upgrade is prepared", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { "check-diff", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_preview, "Check for upgrades and print package diff only", NULL },
  { "preview", 0, 0, G_OPTION_ARG_NONE, &opt_preview, "Just preview package differences", NULL },
  { "check", 0, 0, G_OPTION_ARG_NONE, &opt_check, "Just check if an upgrade is available", NULL },
  { "cache-only", 'C', 0, G_OPTION_ARG_NONE, &opt_cache_only, "Do not download latest ostree and RPM data", NULL },
  { "upgrade-unchanged-exit-77", 0, 0, G_OPTION_ARG_NONE, &opt_upgrade_unchanged_exit_77, "If no upgrade is available, exit 77", NULL },
  { NULL }
};

int
rpmostree_builtin_upgrade (int             argc,
                           char          **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable   *cancellable,
                           GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autofree char *transaction_address = NULL;
  _cleanup_peer_ GPid peer_pid = 0;
  const char *const *install_pkgs = NULL;
  const char *const *uninstall_pkgs = NULL;

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

  if (opt_reboot && opt_preview)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --reboot and --preview");
      return EXIT_FAILURE;
    }

  if (opt_reboot && opt_check)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --reboot and --check");
      return EXIT_FAILURE;
    }

  if (opt_preview && (install_pkgs != NULL || uninstall_pkgs != NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --preview and --install/--uninstall");
      return EXIT_FAILURE;
    }

  /* If both --check and --preview were passed, --preview overrides. */
  if (opt_preview)
    opt_check = FALSE;

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return EXIT_FAILURE;

  g_autoptr(GVariant) previous_deployment = rpmostree_os_dup_default_deployment (os_proxy);

  if (opt_preview || opt_check)
    {
      if (!rpmostree_os_call_download_update_rpm_diff_sync (os_proxy,
                                                            &transaction_address,
                                                            cancellable,
                                                            error))
        return EXIT_FAILURE;
    }
  else
    {
      g_autoptr(GVariant) options =
        rpmostree_get_options_variant (opt_reboot,
                                       opt_allow_downgrade,
                                       opt_cache_only,
                                       FALSE,  /* skip-purge */
                                       FALSE,  /* no-pull-base */
                                       FALSE,  /* dry-run */
                                       FALSE); /* no-overrides */

      /* Use newer D-Bus API only if we have to. */
      if (install_pkgs || uninstall_pkgs)
        {
          if (!rpmostree_update_deployment (os_proxy,
                                            NULL, /* refspec */
                                            NULL, /* revision */
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
          if (!rpmostree_os_call_upgrade_sync (os_proxy,
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

  if (opt_preview || opt_check)
    {
      g_autoptr(GVariant) result = NULL;
      g_autoptr(GVariant) details = NULL;

      if (!rpmostree_os_call_get_cached_update_rpm_diff_sync (os_proxy,
                                                              "",
                                                              &result,
                                                              &details,
                                                              cancellable,
                                                              error))
        return EXIT_FAILURE;

      if (g_variant_n_children (result) == 0)
        return RPM_OSTREE_EXIT_UNCHANGED;

      if (!opt_check)
        rpmostree_print_package_diffs (result);
    }
  else if (!opt_reboot)
    {
      if (!rpmostree_has_new_default_deployment (os_proxy, previous_deployment))
        {
          if (opt_upgrade_unchanged_exit_77)
            return RPM_OSTREE_EXIT_UNCHANGED;
          return EXIT_SUCCESS;
        }

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
