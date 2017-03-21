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

#include <gio/gunixfdlist.h>

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

typedef enum {
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_REBOOT = (1 << 0),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_SKIP_PURGE = (1 << 1),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE = (1 << 2),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_PULL_BASE = (1 << 4),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DRY_RUN = (1 << 5),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NOSCRIPTS = (1 << 6)
} RpmOstreeTransactionDeployFlags;


RpmostreedTransaction *
               rpmostreed_transaction_new_deploy           (GDBusMethodInvocation *invocation,
                                                            OstreeSysroot *sysroot,
                                                            RpmOstreeTransactionDeployFlags flags,
                                                            const char *osname,
                                                            const char *refspec,
                                                            const char *revision,
                                                            const char *const *packages_added,
                                                            const char *const *packages_removed,
                                                            GUnixFDList *local_packages_added,
                                                            GCancellable *cancellable,
                                                            GError **error);

RpmostreedTransaction *
rpmostreed_transaction_new_initramfs_state       (GDBusMethodInvocation *invocation,
                                                  OstreeSysroot         *sysroot,
                                                  const char            *osname,
                                                  gboolean               regenerate,
                                                  char                 **args,
                                                  gboolean               reboot,
                                                  GCancellable          *cancellable,
                                                  GError               **error);

typedef enum {
  RPMOSTREE_TRANSACTION_CLEANUP_BASE = (1 << 0),
  RPMOSTREE_TRANSACTION_CLEANUP_PENDING_DEPLOY = (1 << 1),
  RPMOSTREE_TRANSACTION_CLEANUP_ROLLBACK_DEPLOY = (1 << 2),
  RPMOSTREE_TRANSACTION_CLEANUP_REPOMD = (1 << 3),
} RpmOstreeTransactionCleanupFlags;

RpmostreedTransaction *
rpmostreed_transaction_new_cleanup       (GDBusMethodInvocation *invocation,
                                          OstreeSysroot         *sysroot,
                                          const char            *osname,
                                          RpmOstreeTransactionCleanupFlags flags,
                                          GCancellable          *cancellable,
                                          GError               **error);
