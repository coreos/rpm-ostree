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

#include "libgsystem.h"
#include <libglnx.h>

static char *opt_sysroot = "/";
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
get_args_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "skip-purge", "b", opt_skip_purge);

  return g_variant_dict_end (&dict);
}

gboolean
rpmostree_builtin_rebase (int             argc,
                          char          **argv,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  const char *new_provided_refspec;

  /* forced blank for now */
  const char *packages[] = { NULL };

  GOptionContext *context = g_option_context_new ("REFSPEC - Switch to a different tree");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autofree char *transaction_object_path = NULL;

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_NONE,
                                       cancellable,
                                       &sysroot_proxy,
                                       error))
    goto out;

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    goto out;

  new_provided_refspec = argv[1];

  if (!rpmostree_os_call_rebase_sync (os_proxy,
                                      get_args_variant (),
                                      new_provided_refspec,
                                      packages,
                                      &transaction_object_path,
                                      cancellable,
                                      error))
    goto out;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_object_path,
                                                cancellable,
                                                error))
    goto out;

  if (!opt_reboot)
    {
      /* By request, doing this without dbus */
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
  /* Does nothing if using the message bus. */
  rpmostree_cleanup_peer ();

  return ret;
}
