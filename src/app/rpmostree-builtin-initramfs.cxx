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

static char *opt_osname;
static gboolean opt_enable;
static gboolean opt_disable;
static char **opt_add_arg;
static gboolean opt_reboot;
static gboolean opt_lock_finalization;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "enable", 0, 0, G_OPTION_ARG_NONE, &opt_enable, "Enable regenerating initramfs locally", NULL },
  { "arg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_add_arg, "Append ARG to the dracut arguments", "ARG" },
  { "disable", 0, 0, G_OPTION_ARG_NONE, &opt_disable, "Disable regenerating initramfs locally", NULL },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after operation is complete", NULL },
  { "lock-finalization", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_lock_finalization, "Prevent automatic deployment finalization on shutdown", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_initramfs (int             argc,
                             char          **argv,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable   *cancellable,
                             GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");

  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       NULL,
                                       error))
    return FALSE;

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return FALSE;

  if (!(opt_enable || opt_disable))
    {
      g_autoptr(GVariant) deployments = rpmostree_sysroot_dup_deployments (sysroot_proxy);
      gboolean cur_regenerate = FALSE;
      g_autofree char **initramfs_args = NULL;

      if (opt_reboot)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "--reboot must be used with --enable or --disable");
          return FALSE;
        }
      if (opt_add_arg)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "--arg must be used with --enable");
          return FALSE;
        }

      if (g_variant_n_children (deployments) > 1)
        {
          g_autoptr(GVariant) pending = g_variant_get_child_value (deployments, 0);
          g_auto(GVariantDict) dict;
          g_variant_dict_init (&dict, pending);

          if (!g_variant_dict_lookup (&dict, "regenerate-initramfs", "b", &cur_regenerate))
            cur_regenerate = FALSE;
          if (cur_regenerate)
            {
              g_variant_dict_lookup (&dict, "initramfs-args", "^a&s", &initramfs_args);
            }
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
  else if (opt_enable && opt_disable)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Cannot simultaenously specify --enable and --disable");
      return FALSE;
    }
  else
    {
      char *empty_strv[] = {NULL};
      if (opt_disable && opt_add_arg)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Cannot simultaenously specify --disable and --arg");
          return FALSE;
        }
      if (!opt_add_arg)
        opt_add_arg = empty_strv;

      GVariantDict dict;
      g_variant_dict_init (&dict, NULL);
      g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
      g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
      g_variant_dict_insert (&dict, "lock-finalization", "b", opt_lock_finalization);
      g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

      g_autofree char *transaction_address = NULL;
      if (!rpmostree_os_call_set_initramfs_state_sync (os_proxy,
                                                       opt_enable,
                                                       (const char *const*)opt_add_arg,
                                                       options,
                                                       &transaction_address,
                                                       cancellable,
                                                       error))
        return FALSE;

      if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                    transaction_address,
                                                    cancellable,
                                                    error))
        return FALSE;

      g_print ("Initramfs regeneration is now: %s\n", opt_enable ? "enabled" : "disabled");
    }

  return TRUE;
}
