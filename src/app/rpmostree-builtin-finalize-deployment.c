/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2019 Red Hat, Inc.
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

static char *opt_osname;
static char *opt_expect_checksum;
static gboolean opt_allow_unlocked;

static GOptionEntry option_entries[] = {
  /* though there can only be one staged deployment at a time, this could still
   * be useful to assert a specific osname */
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "expect-checksum", 0, 0, G_OPTION_ARG_STRING, &opt_expect_checksum, "Verify checksum to finalize matches a specific one", "CHECKSUM" },
  { "allow-unlocked", 0, 0, G_OPTION_ARG_NONE, &opt_allow_unlocked, "Don't error out if staged deployment wasn't locked", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_finalize_deployment (int             argc,
                                       char          **argv,
                                       RpmOstreeCommandInvocation *invocation,
                                       GCancellable   *cancellable,
                                       GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");

  _cleanup_peer_ GPid peer_pid = 0;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv,
                                       invocation, cancellable, NULL, NULL,
                                       &sysroot_proxy, &peer_pid, NULL, error))
    return FALSE;

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return FALSE;

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  if (opt_expect_checksum)
    g_variant_dict_insert (&dict, "expect-checksum", "s", opt_expect_checksum);
  g_variant_dict_insert (&dict, "allow-unlocked", "b", opt_allow_unlocked);
  g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

  g_autofree char *transaction_address = NULL;
  if (!rpmostree_os_call_finalize_deployment_sync (os_proxy,
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

  return TRUE;
}
