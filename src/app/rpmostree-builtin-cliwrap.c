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

#include <string.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rust.h"

#include <libglnx.h>

gboolean
rpmostree_builtin_cliwrap (int             argc,
                           char          **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable   *cancellable,
                           GError        **error)
{
  if (argc < 2)
    return glnx_throw (error, "cliwrap: missing required subcommand");

  g_autoptr(GPtrArray) args = g_ptr_array_new ();
  for (int i = 1; i < argc; i++)
    g_ptr_array_add (args, argv[i]);
  g_ptr_array_add (args, NULL);
  return ror_cliwrap_entrypoint ((char**)args->pdata, error);
}
