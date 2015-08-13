/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include "types.h"

Transaction *   transaction_new_package_diff    (GDBusMethodInvocation *invocation,
                                                 OstreeSysroot *sysroot,
                                                 const char *osname,
                                                 const char *refspec,
                                                 GCancellable *cancellable,
                                                 GError **error);

Transaction *   transaction_new_rollback        (GDBusMethodInvocation *invocation,
                                                 OstreeSysroot *sysroot,
                                                 const char *osname,
                                                 GCancellable *cancellable,
                                                 GError **error);

Transaction *   transaction_new_clear_rollback  (GDBusMethodInvocation *invocation,
                                                 OstreeSysroot *sysroot,
                                                 const char *osname,
                                                 GCancellable *cancellable,
                                                 GError **error);

Transaction *   transaction_new_upgrade         (GDBusMethodInvocation *invocation,
                                                 OstreeSysroot *sysroot,
                                                 const char *osname,
                                                 const char *refspec,
                                                 gboolean allow_downgrade,
                                                 gboolean skip_purge,
                                                 GCancellable *cancellable,
                                                 GError **error);
