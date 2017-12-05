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

#pragma once

#include "ostree.h"
#include "rpmostree-dbus-helpers.h"

G_BEGIN_DECLS

/* Exit code for no change after pulling commits.
 * Use alongside EXIT_SUCCESS and EXIT_FAILURE. */
#define RPM_OSTREE_EXIT_UNCHANGED  (77)

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

/* Currently, this has just the command (which is mostly there for the
 * name/flags), but in the future if we want to add something new we won't need
 * to touch every prototype.
 */
struct RpmOstreeCommandInvocation {
  RpmOstreeCommand *command;
};

#define BUILTINPROTO(name) gboolean rpmostree_builtin_ ## name (int argc, char **argv, \
                                                                RpmOstreeCommandInvocation *invocation, \
                                                                GCancellable *cancellable, GError **error)

BUILTINPROTO(compose);
BUILTINPROTO(upgrade);
BUILTINPROTO(reload);
BUILTINPROTO(deploy);
BUILTINPROTO(rebase);
BUILTINPROTO(cancel);
BUILTINPROTO(cleanup);
BUILTINPROTO(rollback);
BUILTINPROTO(initramfs);
BUILTINPROTO(status);
BUILTINPROTO(refresh_md);
BUILTINPROTO(db);
BUILTINPROTO(internals);
BUILTINPROTO(container);
BUILTINPROTO(install);
BUILTINPROTO(uninstall);
BUILTINPROTO(override);
BUILTINPROTO(start_daemon);
BUILTINPROTO(ex);

#undef BUILTINPROTO

const char *rpmostree_subcommand_parse (int *inout_argc,
                                        char **inout_argv,
                                        RpmOstreeCommandInvocation *invocation);

gboolean
rpmostree_option_context_parse (GOptionContext *context,
                                const GOptionEntry *main_entries,
                                int *argc,
                                char ***argv,
                                RpmOstreeCommandInvocation *invocation,
                                GCancellable *cancellable,
                                const char *const* *out_install_pkgs,
                                const char *const* *out_uninstall_pkgs,
                                RPMOSTreeSysroot **out_sysroot_proxy,
                                GPid *out_peer_pid,
                                GError **error);

int
rpmostree_handle_subcommand (int argc, char **argv,
                             RpmOstreeCommand *subcommands,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable *cancellable, GError **error);

G_END_DECLS

