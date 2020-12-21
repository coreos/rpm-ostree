/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat Inc.
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
#include "rpmostree-override-builtins.h"

static RpmOstreeCommand override_subcommands[] = {
  { "replace", RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS,
    "Replace packages in the base layer",
    rpmostree_override_builtin_replace },
  { "remove", RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS,
    "Remove packages from the base layer",
    rpmostree_override_builtin_remove },
  { "reset", RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS,
    "Reset currently active package overrides",
    rpmostree_override_builtin_reset },
  { NULL, (RpmOstreeBuiltinFlags)0, NULL, NULL }
};

gboolean
rpmostree_builtin_override (int argc, char **argv,
                            RpmOstreeCommandInvocation *invocation,
                            GCancellable *cancellable, GError **error)
{
  return rpmostree_handle_subcommand (argc, argv, override_subcommands,
                                      invocation, cancellable, error);
}

