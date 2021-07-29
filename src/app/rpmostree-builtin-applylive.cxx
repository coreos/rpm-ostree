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
#include "rpmostree-clientlib.h"
#include "rpmostree-cxxrs.h"

#include <libglnx.h>

gboolean
rpmostree_ex_builtin_apply_live (int             argc,
                                 char          **argv,
                                 RpmOstreeCommandInvocation *invocation,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  rust::Vec<rust::String> rustargv;
  for (int i = 0; i < argc; i++)
    rustargv.push_back(std::string(argv[i]));
  CXX_TRY(applylive_entrypoint(rustargv), error);
  return TRUE;
}
