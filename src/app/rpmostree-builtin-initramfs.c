/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

#include <libglnx.h>

static gboolean opt_enable;
static gboolean opt_disable;
static char **opt_add_arg;
static gboolean opt_reboot;

static GOptionEntry option_entries[] = {
  { "enable", 0, 0, G_OPTION_ARG_NONE, &opt_enable, "Enable regenerating initramfs locally", NULL },
  { "arg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_add_arg, "Append ARG to the dracut arguments", "ARG" },
  { "disable", 0, 0, G_OPTION_ARG_NONE, &opt_disable, "Disable regenerating initramfs locally", NULL },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after operation is complete", NULL },
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

int
rpmostree_builtin_initramfs (int             argc,
                             char          **argv,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable   *cancellable,
                             GError        **error)
{
  int exit_status = EXIT_FAILURE;
  g_autoptr(GOptionContext) context = g_option_context_new ("- Enable or disable local initramfs regeneration");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autofree char *transaction_address = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       error))
    goto out;

  if (!rpmostree_load_os_proxy (sysroot_proxy, NULL,
                                cancellable, &os_proxy, error))
    goto out;

  if (!(opt_enable || opt_disable))
    {
      GVariantIter iter;
      g_autoptr(GVariant) deployments = rpmostree_sysroot_dup_deployments (sysroot_proxy);

      if (opt_reboot)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "--reboot must be used with --enable or --disable");
          goto out;
        }

      g_variant_iter_init (&iter, deployments);

      while (TRUE)
        {
          gboolean cur_regenerate;
          g_autoptr(GVariant) child = g_variant_iter_next_value (&iter);
          g_autoptr(GVariantDict) dict = NULL;
          g_autofree char **initramfs_args = NULL;
          gboolean is_booted;

          if (child == NULL)
            break;

          dict = g_variant_dict_new (child);

          if (!g_variant_dict_lookup (dict, "booted", "b", &is_booted))
            continue;
          if (!is_booted)
            continue;

          if (!g_variant_dict_lookup (dict, "regenerate-initramfs", "b", &cur_regenerate))
            cur_regenerate = FALSE;
          if (cur_regenerate)
            {
              g_variant_dict_lookup (dict, "initramfs-args", "^a&s", &initramfs_args);
            }

          g_print ("Initramfs regeneration: %s\n", cur_regenerate ? "enabled" : "disabled");
          if (initramfs_args)
            {
              g_print ("Initramfs args: ");
              for (char **iter = initramfs_args; iter && *iter; iter++)
                g_print ("%s ", *iter);
              g_print ("\n");
            }
        }
    }
  else if (opt_enable && opt_disable)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Cannot simultaenously specify --enable and --disable");
      goto out;
    }
  else
    {
      char *empty_strv[] = {NULL};
      if (opt_disable && opt_add_arg)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Cannot simultaenously specify --disable and --arg");
          goto out;
        }
      if (!opt_add_arg)
        opt_add_arg = empty_strv;
      if (!rpmostree_os_call_set_initramfs_state_sync (os_proxy,
                                                       opt_enable,
                                                       (const char *const*)opt_add_arg,
                                                       get_args_variant (),
                                                       &transaction_address,
                                                       cancellable,
                                                       error))
        goto out;

      if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                    transaction_address,
                                                    cancellable,
                                                    error))
        goto out;

      g_print ("Initramfs regeneration is now: %s\n", opt_enable ? "enabled" : "disabled");
    }

  exit_status = EXIT_SUCCESS;

out:
  return exit_status;
}
