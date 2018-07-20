/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

#include "rpmostree-container-builtins.h"
#include "rpmostree-rpm-util.h"

static RpmOstreeCommand container_subcommands[] = {
  { "init", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Initialize a local container", rpmostree_container_builtin_init },
  { "assemble", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Assemble a local container", rpmostree_container_builtin_assemble },
  { "mkrootfs", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Generate a root filesystem", rpmostree_container_builtin_mkrootfs },
  /* { "start", rpmostree_container_builtin_start }, */
  { "upgrade", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Perform a local container upgrade", rpmostree_container_builtin_upgrade },
  { NULL, 0, NULL, NULL }
};

gboolean
rpmostree_builtin_container (int argc, char **argv,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable *cancellable, GError **error)
{
  return rpmostree_handle_subcommand (argc, argv, container_subcommands,
                                      invocation, cancellable, error);
}

