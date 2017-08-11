/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include <gio/gunixfdlist.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_dry_run;
static gchar **opt_install;
static gchar **opt_uninstall;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after upgrade is prepared", NULL },
  { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &opt_dry_run, "Exit after printing the transaction", NULL },
  { NULL }
};

static GOptionEntry install_option_entry[] = {
  { "install", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_install, "Install a package", "PKG" },
  { NULL }
};

static GOptionEntry uninstall_option_entry[] = {
  { "uninstall", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_uninstall, "Uninstall a package", "PKG" },
  { NULL }
};

static int
pkg_change (RPMOSTreeSysroot *sysroot_proxy,
            const char *const* packages_to_add,
            const char *const* packages_to_remove,
            GCancellable  *cancellable,
            GError       **error)
{
  const char *const strv_empty[] = { NULL };

  if (!packages_to_add)
    packages_to_add = strv_empty;
  if (!packages_to_remove)
    packages_to_remove = strv_empty;

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return EXIT_FAILURE;

  g_autoptr(GVariant) options =
    rpmostree_get_options_variant (opt_reboot,
                                   FALSE,   /* allow-downgrade */
                                   FALSE,   /* skip-purge */
                                   TRUE,    /* no-pull-base */
                                   opt_dry_run,
                                   FALSE);  /* no-overrides */

  gboolean met_local_pkg = FALSE;
  for (const char *const* it = packages_to_add; it && *it; it++)
    met_local_pkg = met_local_pkg || g_str_has_suffix (*it, ".rpm");

  /* Use newer D-Bus API only if we have to. */
  g_autofree char *transaction_address = NULL;
  if (met_local_pkg)
    {
      if (!rpmostree_update_deployment (os_proxy,
                                        NULL, /* refspec */
                                        NULL, /* revision */
                                        packages_to_add,
                                        packages_to_remove,
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
      if (!rpmostree_os_call_pkg_change_sync (os_proxy,
                                              options,
                                              packages_to_add,
                                              packages_to_remove,
                                              NULL,
                                              &transaction_address,
                                              NULL,
                                              cancellable,
                                              error))
        return EXIT_FAILURE;
    }

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return EXIT_FAILURE;

  if (opt_dry_run)
    {
      g_print ("Exiting because of '--dry-run' option\n");
    }
  else if (!opt_reboot)
    {
      const char *sysroot_path;


      sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);

      if (!rpmostree_print_treepkg_diff_from_sysroot_path (sysroot_path,
                                                           cancellable,
                                                           error))
        return EXIT_FAILURE;

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  return EXIT_SUCCESS;
}

int
rpmostree_builtin_install (int            argc,
                           char         **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable  *cancellable,
                           GError       **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, uninstall_option_entry, NULL);

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       error))
    return EXIT_FAILURE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return EXIT_FAILURE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++; argc--;
  argv[argc] = NULL;

  return pkg_change (sysroot_proxy,
                     (const char *const*)argv,
                     (const char *const*)opt_uninstall,
                     cancellable, error);
}

int
rpmostree_builtin_uninstall (int            argc,
                             char         **argv,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable  *cancellable,
                             GError       **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, install_option_entry, NULL);

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       error))
    return EXIT_FAILURE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return EXIT_FAILURE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++; argc--;
  argv[argc] = NULL;

  return pkg_change (sysroot_proxy,
                     (const char *const*)opt_install,
                     (const char *const*)argv,
                     cancellable, error);
}
