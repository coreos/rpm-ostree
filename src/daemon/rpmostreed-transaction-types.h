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
#include "rpmostreed-daemon.h"
#include "rpmostreed-os.h"

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
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE = (1 << 2),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_PULL_BASE = (1 << 4),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DRY_RUN = (1 << 5),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_ONLY = (1 << 8),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DOWNLOAD_METADATA_ONLY = (1 << 9),
  RPMOSTREE_TRANSACTION_DEPLOY_FLAG_STAGE = (1 << 10),
} RpmOstreeTransactionDeployFlags;


RpmostreedTransaction *
rpmostreed_transaction_new_deploy (GDBusMethodInvocation *invocation,
                                   OstreeSysroot *sysroot,
                                   const char *osname,
                                   RpmOstreeTransactionDeployFlags flags,
                                   GVariant               *options,
                                   RpmOstreeUpdateDeploymentModifiers *modifiers,
                                   GUnixFDList            *fd_list,
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

typedef enum {
  RPMOSTREE_TRANSACTION_LIVEFS_FLAG_DRY_RUN = (1 << 0),
  RPMOSTREE_TRANSACTION_LIVEFS_FLAG_REPLACE = (1 << 1),
} RpmOstreeTransactionLiveFsFlags;

RpmostreedTransaction *
rpmostreed_transaction_new_livefs (GDBusMethodInvocation *invocation,
                                   OstreeSysroot         *sysroot,
                                   RpmOstreeTransactionLiveFsFlags flags,
                                   GCancellable          *cancellable,
                                   GError               **error);

typedef enum {
  RPMOSTREE_TRANSACTION_REFRESH_MD_FLAG_FORCE = (1 << 0),
} RpmOstreeTransactionRefreshMdFlags;

RpmostreedTransaction *
rpmostreed_transaction_new_refresh_md (GDBusMethodInvocation *invocation,
                                       OstreeSysroot         *sysroot,
                                       RpmOstreeTransactionRefreshMdFlags flags,
                                       const char            *osname,
                                       GCancellable          *cancellable,
                                       GError               **error);
typedef enum {
  RPMOSTREE_TRANSACTION_KERNEL_ARG_FLAG_REBOOT = (1 << 0),
} RpmOstreeTransactionKernelArgFlags;

RpmostreedTransaction *
rpmostreed_transaction_new_kernel_arg (GDBusMethodInvocation *invocation,
                                       OstreeSysroot         *sysroot,
                                       const char *           osname,
                                       const char *           existing_kernel_args,
                                       const char * const *kernel_args_added,
                                       const char * const *kernel_args_replaced,
                                       const char * const *kernel_args_deleted,
                                       RpmOstreeTransactionKernelArgFlags flags,
                                       GCancellable          *cancellable,
                                       GError               **error);

