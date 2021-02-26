/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2020 Jonathan Lebon <jonathan@jlebon.com>
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

#include "rpmostree-ex-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_force_sync;
static char **opt_track;
static char **opt_untrack;
static gboolean opt_untrack_all;
static gboolean opt_reboot;
static gboolean opt_lock_finalization;
static gboolean opt_unchanged_exit_77;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "force-sync", 0, 0, G_OPTION_ARG_NONE, &opt_force_sync, "Deploy a new tree with the latest tracked /etc files", NULL },
  { "track", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_track, "Track root /etc file", "FILE" },
  { "untrack", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_untrack, "Untrack root /etc file", "FILE" },
  { "untrack-all", 0, 0, G_OPTION_ARG_NONE, &opt_untrack_all, "Untrack all root /etc files", NULL },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after operation is complete", NULL },
  { "lock-finalization", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_lock_finalization, "Prevent automatic deployment finalization on shutdown", NULL },
  { "unchanged-exit-77", 0, 0, G_OPTION_ARG_NONE, &opt_unchanged_exit_77, "If no new deployment made, exit 77", NULL },

  { NULL }
};

gboolean
rpmostree_ex_builtin_initramfs_etc (int             argc,
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

  g_autoptr(GVariant) previous_deployment = rpmostree_os_dup_default_deployment (os_proxy);

  if (!(opt_track || opt_untrack || opt_untrack_all || opt_force_sync))
    {
      if (opt_reboot)
        return glnx_throw (error, "Cannot use ---reboot without --track, --untrack, --untrack-all, or --force-sync");

      g_autofree char **files = NULL;
      g_autoptr(GVariant) deployments = rpmostree_sysroot_dup_deployments (sysroot_proxy);
      if (g_variant_n_children (deployments) > 0)
        {
          g_autoptr(GVariant) pending = g_variant_get_child_value (deployments, 0);
          g_auto(GVariantDict) dict;
          g_variant_dict_init (&dict, pending);

          g_variant_dict_lookup (&dict, "initramfs-etc", "^a&s", &files);
        }

      if (!files || !*files)
        g_print ("No tracked files.\n");
      else
        {
          g_print ("Tracked files:\n");
          for (char **it = files; it && *it; it++)
            g_print ("  %s\n", *it);
        }

      return TRUE; /* note early return */
    }

  char *empty_strv[] = {NULL};
  if (!opt_track)
    opt_track = empty_strv;
  if (!opt_untrack)
    opt_untrack = empty_strv;

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
  g_variant_dict_insert (&dict, "lock-finalization", "b", opt_lock_finalization);
  g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

  g_autofree char *transaction_address = NULL;
  if (!rpmostree_os_call_initramfs_etc_sync (os_proxy,
                                             (const char *const*)opt_track,
                                             (const char *const*)opt_untrack,
                                             opt_untrack_all,
                                             opt_force_sync,
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

  if (!opt_reboot)
    {
      if (!rpmostree_has_new_default_deployment (os_proxy, previous_deployment))
        {
          if (opt_unchanged_exit_77)
            invocation->exit_code = RPM_OSTREE_EXIT_UNCHANGED;
          return TRUE;
        }

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  return TRUE;
}
