/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

#pragma once

#include "ostree.h"

G_BEGIN_DECLS

/* Exit code for no change after pulling commits.
 * Use alongside EXIT_SUCCESS and EXIT_FAILURE. */
#define RPM_OSTREE_EXIT_UNCHANGED  (77)

/* Exit code for when a pending deployment can be rebooted into. */
#define RPM_OSTREE_EXIT_PENDING  (77)

typedef enum {
  RPM_OSTREE_BUILTIN_FLAG_NONE = 0,
  RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD = 1 << 0,
  RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT = 1 << 1,
  RPM_OSTREE_BUILTIN_FLAG_HIDDEN = 1 << 2,
  RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS = 1 << 3,
} RpmOstreeBuiltinFlags;

typedef struct RpmOstreeCommand RpmOstreeCommand;
typedef struct RpmOstreeCommandInvocation RpmOstreeCommandInvocation;

struct RpmOstreeCommand {
  const char *name;
  RpmOstreeBuiltinFlags flags;
  const char *description; /* a short decription to describe the functionality */
  int (*fn) (int argc, char **argv, RpmOstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error);
};

/* @command: Passed from core cmdline parsing to cmds
 * @exit_code: Set by commands; default -1 meaning "if GError is set exit 1, otherwise 0"
 */
struct RpmOstreeCommandInvocation {
  RpmOstreeCommand *command;
  const char *command_line;
  int exit_code;
};
G_END_DECLS

