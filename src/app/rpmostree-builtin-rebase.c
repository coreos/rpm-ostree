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
#include "rpmostree-util.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_skip_purge;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after rebase is finished", NULL },
  { "skip-purge", 0, 0, G_OPTION_ARG_NONE, &opt_skip_purge, "Keep previous refspec after rebase", NULL },
  { NULL }
};

static GVariant *
get_args_variant (const char *revision)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "skip-purge", "b", opt_skip_purge);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);

  if (revision != NULL)
    g_variant_dict_insert (&dict, "revision", "s", revision);

  return g_variant_dict_end (&dict);
}

int
rpmostree_builtin_rebase (int             argc,
                          char          **argv,
                          RpmOstreeCommandInvocation *invocation,
                          GCancellable   *cancellable,
                          GError        **error)
{
  const char *new_provided_refspec;
  const char *revision = NULL;

  /* forced blank for now */
  const char *packages[] = { NULL };

  g_autoptr(GOptionContext) context = g_option_context_new ("REFSPEC [REVISION] - Switch to a different tree");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  g_autofree char *transaction_address = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       error))
    return EXIT_FAILURE;

  if (argc < 2 || argc > 3)
    {
      rpmostree_usage_error (context, "Too few or too many arguments", error);
      return EXIT_FAILURE;
    }

  new_provided_refspec = argv[1];

  if (argc == 3)
    revision = argv[2];

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return EXIT_FAILURE;

  if (!rpmostree_os_call_rebase_sync (os_proxy,
                                      get_args_variant (revision),
                                      new_provided_refspec,
                                      packages,
                                      NULL,
                                      &transaction_address,
                                      NULL,
                                      cancellable,
                                      error))
    return EXIT_FAILURE;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return EXIT_FAILURE;

  if (!opt_reboot)
    {
      const char *sysroot_path;

      sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);

      /* By request, doing this without dbus */
      if (!rpmostree_print_treepkg_diff_from_sysroot_path (sysroot_path,
                                                           cancellable,
                                                           error))
        return EXIT_FAILURE;

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  return EXIT_SUCCESS;
}
