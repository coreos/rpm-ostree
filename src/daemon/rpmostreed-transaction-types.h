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

#include "rpmostreed-types.h"

RpmostreedTransaction *
                rpmostreed_transaction_new_package_diff    (GDBusMethodInvocation *invocation,
                                                            OstreeSysroot *sysroot,
                                                            const char *osname,
                                                            const char *refspec,
                                                            const char *revision,
                                                            GCancellable *cancellable,
                                                            GError **error);

RpmostreedTransaction *
                rpmostreed_transaction_new_rollback        (GDBusMethodInvocation *invocation,
                                                            OstreeSysroot *sysroot,
                                                            const char *osname,
                                                            gboolean reboot,
                                                            GCancellable *cancellable,
                                                            GError **error);

RpmostreedTransaction *
                rpmostreed_transaction_new_clear_rollback  (GDBusMethodInvocation *invocation,
                                                            OstreeSysroot *sysroot,
                                                            const char *osname,
                                                            gboolean reboot,
                                                            GCancellable *cancellable,
                                                            GError **error);

RpmostreedTransaction *
               rpmostreed_transaction_new_deploy           (GDBusMethodInvocation *invocation,
                                                            OstreeSysroot *sysroot,
                                                            const char *osname,
                                                            gboolean allow_downgrade,
                                                            const char *refspec,
                                                            const char *revision,
                                                            gboolean skip_purge,
                                                            gboolean reboot,
                                                            GCancellable *cancellable,
                                                            GError **error);

typedef enum {
  RPMOSTREE_TRANSACTION_PKG_FLAG_REBOOT = (1 << 0),
  RPMOSTREE_TRANSACTION_PKG_FLAG_DRY_RUN = (1 << 1),
  RPMOSTREE_TRANSACTION_PKG_FLAG_NOSCRIPTS = (1 << 2)
} RpmOstreeTransactionPkgFlags;

RpmostreedTransaction *
               rpmostreed_transaction_new_pkg_change       (GDBusMethodInvocation *invocation,
                                                            OstreeSysroot         *sysroot,
                                                            const char            *osname,
                                                            const char *const     *packages_added,
                                                            const char *const     *packages_removed,
                                                            const char *const     *ignore_scripts,
							    RpmOstreeTransactionPkgFlags flags,
                                                            GCancellable          *cancellable,
                                                            GError               **error);
RpmostreedTransaction *
rpmostreed_transaction_new_initramfs_state       (GDBusMethodInvocation *invocation,
                                                  OstreeSysroot         *sysroot,
                                                  const char            *osname,
                                                  gboolean               regenerate,
                                                  char                 **args,
                                                  gboolean               reboot,
                                                  GCancellable          *cancellable,
                                                  GError               **error);
