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
#include "rpmostree-cxxrs.h"

#include <libglnx.h>

static char *opt_target;
static gboolean opt_reset;

static GOptionEntry option_entries[] = {
  { "target", 0, 0, G_OPTION_ARG_STRING, &opt_target, "Target provided commit instead of pending deployment", NULL },
  { "reset", 0, 0, G_OPTION_ARG_NONE, &opt_reset, "Reset back to booted commit", NULL },
  { NULL }
};

static GVariant *
get_args_variant (GError **error)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  if (opt_target)
    {
      if (opt_reset)
        return (GVariant*)glnx_null_throw (error, "Cannot specify both --target and --reset");
      g_variant_dict_insert (&dict, "target", "s", opt_target);
    }
  else if (opt_reset)
    {
      OstreeSysroot *sysroot = ostree_sysroot_new_default ();
      if (!ostree_sysroot_load (sysroot, NULL, error))
        return FALSE;
      OstreeDeployment *booted = ostree_sysroot_get_booted_deployment (sysroot);
      if (!booted)
        return (GVariant*)glnx_null_throw (error, "Not in a booted OSTree deployment");
      g_variant_dict_insert (&dict, "target", "s", ostree_deployment_get_csum (booted));
    }

  return g_variant_ref_sink (g_variant_dict_end (&dict));
}

gboolean
rpmostree_ex_builtin_apply_live (int             argc,
                                 char          **argv,
                                 RpmOstreeCommandInvocation *invocation,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new ("");
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
  glnx_unref_object RPMOSTreeOSExperimental *osexperimental_proxy = NULL;
  if (!rpmostree_load_os_proxies (sysroot_proxy, NULL,
                                  cancellable, &os_proxy,
                                  &osexperimental_proxy, error))
    return FALSE;

  g_autofree char *transaction_address = NULL;
  g_autoptr(GVariant) args = get_args_variant (error);
  if (!args)
    return FALSE;
  if (!rpmostree_osexperimental_call_live_fs_sync (osexperimental_proxy,
                                                   args,
                                                   &transaction_address,
                                                   cancellable,
                                                   error))
    return FALSE;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return FALSE;

  rpmostreecxx::applylive_client_finish();

  return TRUE;
}
