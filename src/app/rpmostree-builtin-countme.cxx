/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2020 Red Hat, Inc.
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
#include "rpmostree-cxxrs.h"

#include <libglnx.h>

gboolean
rpmostree_builtin_countme (int             argc,
                           char          **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable   *cancellable,
                           GError        **error)
{
  if (argc > 1)
    return glnx_throw (error, "countme: No argument supported");

  rust::Vec<rust::String> rustargv;
  for (int i = 1; i < argc; i++)
    rustargv.push_back(std::string(argv[i]));
  rpmostreecxx::countme_entrypoint (rustargv);
  return TRUE;
}
