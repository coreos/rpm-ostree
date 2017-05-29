/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "rpmostree-compose-builtins.h"
#include "rpmostree-builtins.h"

#include <ostree.h>
#include "libglnx.h"

#include <glib/gi18n.h>

static RpmOstreeCommand compose_subcommands[] = {
  { "tree", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD | RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    rpmostree_compose_builtin_tree },
  { NULL, 0, NULL }
};

int
rpmostree_builtin_compose (int argc, char **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  return rpmostree_handle_subcommand (argc, argv, compose_subcommands,
                                      invocation, cancellable, error);
}
