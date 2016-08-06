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
#include <gio/gio.h>

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"

#include <libglnx.h>

static GOptionEntry option_entries[] = {
  { NULL }
};

int
rpmostree_builtin_reload (int             argc,
                          char          **argv,
                          GCancellable   *cancellable,
                          GError        **error)
{
  int exit_status = EXIT_FAILURE;
  g_autoptr(GOptionContext) context = g_option_context_new ("- Reload configuration");
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
                                       cancellable,
                                       &sysroot_proxy,
                                       error))
    goto out;

  if (!rpmostree_sysroot_call_reload_config_sync (sysroot_proxy, cancellable, error))
    goto out;

  exit_status = EXIT_SUCCESS;
out:
  /* Does nothing if using the message bus. */
  rpmostree_cleanup_peer ();
  return exit_status;
}
