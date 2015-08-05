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

typedef enum {
  RPM_OSTREE_BUILTIN_FLAG_NONE = 0,
  RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD = 1 << 0
} RpmOstreeBuiltinFlags;

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, GCancellable *cancellable, GError **error);
} RpmOstreeCommand;

#define BUILTINPROTO(name) gboolean rpmostree_builtin_ ## name (int argc, char **argv, GCancellable *cancellable, GError **error)

BUILTINPROTO(compose);
BUILTINPROTO(upgrade);
BUILTINPROTO(rebase);
BUILTINPROTO(rollback);
BUILTINPROTO(status);
BUILTINPROTO(db);

#undef BUILTINPROTO

gboolean rpmostree_option_context_parse (GOptionContext *context,
                                         const GOptionEntry *main_entries,
                                         int *argc,
                                         char ***argv,
                                         RpmOstreeBuiltinFlags flags,
                                         GCancellable *cancellable,
                                         RPMOSTreeSysroot **out_sysroot_proxy,
                                         GError **error);

void rpmostree_print_gpg_verify_result (OstreeGpgVerifyResult *result);

G_END_DECLS

