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

/* "check-diff" is deprecated, replaced by "preview" */
static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after an upgrade is prepared", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { "check-diff", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_preview, "Check for upgrades and print package diff only", NULL },
  { "preview", 0, 0, G_OPTION_ARG_NONE, &opt_preview, "Just preview package differences", NULL },
  { "check", 0, 0, G_OPTION_ARG_NONE, &opt_check, "Just check if an upgrade is available", NULL },
  { "upgrade-unchanged-exit-77", 0, 0, G_OPTION_ARG_NONE, &opt_upgrade_unchanged_exit_77, "If no upgrade is available, exit 77", NULL },
  { NULL }
};

static GVariant *
get_args_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "allow-downgrade", "b", opt_allow_downgrade);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);

  return g_variant_dict_end (&dict);
}

int
rpmostree_builtin_upgrade (int             argc,
                           char          **argv,
                           GCancellable   *cancellable,
                           GError        **error)
{
  int exit_status = EXIT_FAILURE;

  GOptionContext *context = g_option_context_new ("- Perform a system upgrade");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GVariant) previous_default_deployment = NULL;
  g_autoptr(GVariant) new_default_deployment = NULL;
  g_autofree char *transaction_address = NULL;

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_NONE,
                                       cancellable,
                                       &sysroot_proxy,
                                       error))
    goto out;

  if (opt_reboot && opt_preview)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --reboot and --preview");
      goto out;
    }

  if (opt_reboot && opt_check)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --reboot and --check");
      goto out;
    }

  /* If both --check and --preview were passed, --preview overrides. */
  if (opt_preview)
    opt_check = FALSE;

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    goto out;

  previous_default_deployment = rpmostree_os_dup_default_deployment (os_proxy);

  if (opt_preview || opt_check)
    {
      if (!rpmostree_os_call_download_update_rpm_diff_sync (os_proxy,
                                                            &transaction_address,
                                                            cancellable,
                                                            error))
        goto out;
    }
  else
    {
      if (!rpmostree_os_call_upgrade_sync (os_proxy,
                                           get_args_variant (),
                                           &transaction_address,
                                           cancellable,
                                           error))
        goto out;
    }

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    goto out;

  new_default_deployment = rpmostree_os_dup_default_deployment (os_proxy);

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
        goto out;

      if (g_variant_n_children (result) == 0)
        {
          exit_status = RPM_OSTREE_EXIT_UNCHANGED;
          goto out;
        }

      if (!opt_check)
        rpmostree_print_package_diffs (result);
    }
  else if (!opt_reboot)
    {
      const char *sysroot_path;
      gboolean changed;

      sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);

      if (previous_default_deployment == new_default_deployment)
        changed = FALSE;
      else
        changed = !g_variant_equal (previous_default_deployment, new_default_deployment);

      if (!changed)
        {
          if (opt_upgrade_unchanged_exit_77 && !changed)
            exit_status = RPM_OSTREE_EXIT_UNCHANGED;
          else
            exit_status = EXIT_SUCCESS;
          goto out;
        }

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
