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

#define RPMOSTREED_TYPE_TRANSACTION          (rpmostreed_transaction_get_type ())
#define RPMOSTREED_TRANSACTION(o)            (G_TYPE_CHECK_INSTANCE_CAST ((o), RPMOSTREED_TYPE_TRANSACTION, RpmostreedTransaction))
#define RPMOSTREED_TRANSACTION_CLASS(c)      (G_TYPE_CHECK_CLASS_CAST ((c), RPMOSTREED_TYPE_TRANSACTION, RpmostreedTransactionClass))
#define RPMOSTREED_IS_TRANSACTION(o)         (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPMOSTREED_TYPE_TRANSACTION))
#define RPMOSTREED_TRANSACTION_GET_CLASS(o)  (G_TYPE_INSTANCE_GET_CLASS ((o), RPMOSTREED_TYPE_TRANSACTION, RpmostreedTransactionClass))

typedef struct _RpmostreedTransactionClass RpmostreedTransactionClass;
typedef struct _RpmostreedTransactionPrivate RpmostreedTransactionPrivate;

struct _RpmostreedTransaction {
  RPMOSTreeTransactionSkeleton parent;
  RpmostreedTransactionPrivate *priv;
};

struct _RpmostreedTransactionClass {
  RPMOSTreeTransactionSkeletonClass parent_class;

  gboolean      (*execute)                 (RpmostreedTransaction *transaction,
                                            GCancellable *cancellable,
                                            GError **error);
};

GType           rpmostreed_transaction_get_type            (void) G_GNUC_CONST;
gboolean        rpmostreed_transaction_get_active          (RpmostreedTransaction *transaction);
OstreeSysroot * rpmostreed_transaction_get_sysroot         (RpmostreedTransaction *transaction);
GDBusMethodInvocation *
                rpmostreed_transaction_get_invocation      (RpmostreedTransaction *transaction);
const char *    rpmostreed_transaction_get_client_address  (RpmostreedTransaction *transaction);
gboolean        rpmostreed_transaction_is_compatible       (RpmostreedTransaction *transaction,
                                                            GDBusMethodInvocation *invocation);
void            rpmostreed_transaction_connect_download_progress
                                                           (RpmostreedTransaction *transaction,
                                                            OstreeAsyncProgress *progress);
void            rpmostreed_transaction_connect_signature_progress
                                                           (RpmostreedTransaction *transaction,
                                                            OstreeRepo *repo);
void            rpmostreed_transaction_force_close         (RpmostreedTransaction *transaction);
