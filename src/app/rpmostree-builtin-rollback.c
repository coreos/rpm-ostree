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

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"

#include "libgsystem.h"
#include <libglnx.h>

static char *opt_sysroot = "/";
static gboolean opt_reboot;
static gboolean opt_force_peer;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after rollback is prepared", NULL },
  { "peer", 0, 0, G_OPTION_ARG_NONE, &opt_force_peer, "Force a peer to peer connection instead of using the system message bus", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_rollback (int             argc,
                            char          **argv,
                            GCancellable   *cancellable,
                            GError        **error)
{
  gboolean ret = FALSE;
  gboolean is_peer = FALSE;

  GOptionContext *context = g_option_context_new ("- Revert to the previously booted tree");
  glnx_unref_object GDBusConnection *connection = NULL;
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autofree char *transaction_object_path = NULL;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  if (!rpmostree_load_connection_and_sysroot (opt_sysroot,
                                              opt_force_peer,
                                              cancellable,
                                              &connection,
                                              &sysroot_proxy,
                                              &is_peer,
                                              error))
    goto out;

  if (!rpmostree_load_os_proxy (sysroot_proxy, NULL, is_peer,
                                cancellable, &os_proxy, error))
    goto out;

  if (!rpmostree_os_call_rollback_sync (os_proxy,
                                        &transaction_object_path,
                                        cancellable,
                                        error))
    goto out;

  if (!rpmostree_transaction_get_response_sync (connection,
                                                transaction_object_path,
                                                cancellable,
                                                error))
    goto out;

  if (!opt_reboot)
    {
      // by request, doing this without dbus
      if (!rpmostree_print_treepkg_diff_from_sysroot_path (opt_sysroot,
                                                           cancellable,
                                                           error))
        goto out;

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }
  else
    {
      gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                     cancellable, error,
                                     "systemctl", "reboot", NULL);
    }

  ret = TRUE;

out:
  if (is_peer)
    rpmostree_cleanup_peer ();

  return ret;
}
