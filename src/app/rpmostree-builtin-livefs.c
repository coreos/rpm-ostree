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

#include "rpmostree-ex-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"

#include <libglnx.h>

static char *opt_target;

static GOptionEntry option_entries[] = {
  { "target", 0, 0, G_OPTION_ARG_NONE, &opt_target, "Target provided commit instead of pending deployment", NULL },
  { NULL }
};

static GVariant *
get_args_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  if (opt_target)
    g_variant_dict_insert (&dict, "target", "s", opt_target);

  return g_variant_dict_end (&dict);
}

gboolean
rpmostree_ex_builtin_livefs (int             argc,
                             char          **argv,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable   *cancellable,
                             GError        **error)
{
  _cleanup_peer_ GPid peer_pid = 0;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       NULL,
                                       error))
    return FALSE;

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeOSExperimental *osexperimental_proxy = NULL;
  if (!rpmostree_load_os_proxies (sysroot_proxy, NULL,
                                  cancellable, &os_proxy,
                                  &osexperimental_proxy, error))
    return FALSE;

  g_autofree char *transaction_address = NULL;
  if (!rpmostree_osexperimental_call_live_fs_sync (osexperimental_proxy,
                                                   get_args_variant (),
                                                   &transaction_address,
                                                   cancellable,
                                                   error))
    return FALSE;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return FALSE;

  return TRUE;
}
