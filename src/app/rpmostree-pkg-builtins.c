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

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_dry_run;
/* Turn off the noscripts stuff for now, since we aren't persisting
 * it, and I hope we can mostly get away with not needing it.
 */
#if 0
static gboolean opt_no_scripts;
static char **opt_ignore_script;
#endif

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after upgrade is prepared", NULL },
  { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &opt_dry_run, "Exit after printing the transaction", NULL },
#if 0
  { "noscripts", 0, 0, G_OPTION_ARG_NONE, &opt_no_scripts, "Do not run scripts", NULL },
  { "ignore-script", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_ignore_script, "Ignore a script for RPM", NULL },
#endif
  { NULL }
};

static GVariant *
get_args_variant (void)
{
  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "dry-run", "b", opt_dry_run);
#if 0
  if (opt_no_scripts)
      g_variant_dict_insert (&dict, "noscripts", "b", TRUE);
  if (opt_ignore_script)
    g_variant_dict_insert (&dict, "ignore-scripts", "^as", opt_ignore_script);
#endif
  return g_variant_dict_end (&dict);
}

static int
pkg_change (RPMOSTreeSysroot *sysroot_proxy,
            const char *const* packages_to_add,
            const char *const* packages_to_remove,
            GCancellable  *cancellable,
            GError       **error)
{
  int exit_status = EXIT_FAILURE;
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  g_autofree char *transaction_address = NULL;
  const char *const strv_empty[] = { NULL };

  if (!packages_to_add)
    packages_to_add = strv_empty;
  if (!packages_to_remove)
    packages_to_remove = strv_empty;

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    goto out;

  if (!rpmostree_os_call_pkg_change_sync (os_proxy,
                                          get_args_variant (),
                                          packages_to_add,
                                          packages_to_remove,
                                          &transaction_address,
                                          cancellable,
                                          error))
    goto out;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    goto out;

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
        goto out;

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  exit_status = EXIT_SUCCESS;

out:
  /* Does nothing if using the message bus. */
  rpmostree_cleanup_peer ();

  return exit_status;
}

int
rpmostree_builtin_pkg_add (int            argc,
                           char         **argv,
                           GCancellable  *cancellable,
                           GError       **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GPtrArray) packages_to_add = g_ptr_array_new ();

  context = g_option_context_new ("PACKAGE [PACKAGE...] - Download and install layered RPM packages");

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_NONE,
                                       cancellable,
                                       &sysroot_proxy,
                                       error))
    return EXIT_FAILURE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return EXIT_FAILURE;
    }

  for (int i = 1; i < argc; i++)
    g_ptr_array_add (packages_to_add, argv[i]);
  g_ptr_array_add (packages_to_add, NULL);

  return pkg_change (sysroot_proxy, (const char *const*)packages_to_add->pdata, NULL, cancellable, error);
}

int
rpmostree_builtin_pkg_remove (int            argc,
                              char         **argv,
                              GCancellable  *cancellable,
                              GError       **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GPtrArray) packages_to_remove = g_ptr_array_new ();

  context = g_option_context_new ("PACKAGE [PACKAGE...] - Remove one or more overlay packages");

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_NONE,
                                       cancellable,
                                       &sysroot_proxy,
                                       error))
    return EXIT_FAILURE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return EXIT_FAILURE;
    }

  for (int i = 1; i < argc; i++)
    g_ptr_array_add (packages_to_remove, argv[i]);
  g_ptr_array_add (packages_to_remove, NULL);

  return pkg_change (sysroot_proxy, NULL, (const char *const*)packages_to_remove->pdata, cancellable, error);
}
