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

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after upgrade is prepared", NULL },
  { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &opt_dry_run, "Exit after printing the transaction", NULL },
  { NULL }
};

static GVariant *
get_args_variant (GVariant *handles)
{
  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "dry-run", "b", opt_dry_run);
  if (handles != NULL)
    g_variant_dict_insert_value (&dict, "install-local-packages", handles);
  return g_variant_dict_end (&dict);
}

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

  g_autoptr(GPtrArray) repo_pkgs = NULL;
  g_autoptr(GVariant) local_pkgs_fd_idxs = NULL;
  glnx_unref_object GUnixFDList *local_pkgs_fd_list = NULL;
  if (!rpmostree_sort_pkgs_strv (packages_to_add,
                                 &repo_pkgs, &local_pkgs_fd_list,
                                 &local_pkgs_fd_idxs, error))
    return EXIT_FAILURE;

  g_ptr_array_add (repo_pkgs, NULL);

  g_autofree char *transaction_address = NULL;
  if (!rpmostree_os_call_pkg_change_sync (os_proxy,
                                          get_args_variant (local_pkgs_fd_idxs),
                                          (const char *const*)repo_pkgs->pdata,
                                          packages_to_remove,
                                          local_pkgs_fd_list,
                                          &transaction_address,
                                          NULL, /* out_fd_list */
                                          cancellable,
                                          error))
    return EXIT_FAILURE;

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
rpmostree_builtin_pkg_add (int            argc,
                           char         **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable  *cancellable,
                           GError       **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  context = g_option_context_new ("PACKAGE [PACKAGE...] - Download and install layered RPM packages");

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
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

  return pkg_change (sysroot_proxy, (const char *const*)argv,
                     NULL, cancellable, error);
}

int
rpmostree_builtin_pkg_remove (int            argc,
                              char         **argv,
                              RpmOstreeCommandInvocation *invocation,
                              GCancellable  *cancellable,
                              GError       **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  context = g_option_context_new ("PACKAGE [PACKAGE...] - Remove one or more overlay packages");

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
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

  return pkg_change (sysroot_proxy, NULL, (const char *const*)argv,
                     cancellable, error);
}
