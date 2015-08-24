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

#define TYPE_TRANSACTION          (transaction_get_type ())
#define TRANSACTION(o)            (G_TYPE_CHECK_INSTANCE_CAST ((o), TYPE_TRANSACTION, Transaction))
#define TRANSACTION_CLASS(c)      (G_TYPE_CHECK_CLASS_CAST ((c), TYPE_TRANSACTION, TransactionClass))
#define IS_TRANSACTION(o)         (G_TYPE_CHECK_INSTANCE_TYPE ((o), TYPE_TRANSACTION))
#define TRANSACTION_GET_CLASS(o)  (G_TYPE_INSTANCE_GET_CLASS ((o), TYPE_TRANSACTION, TransactionClass))

typedef struct _TransactionClass TransactionClass;
typedef struct _TransactionPrivate TransactionPrivate;

struct _Transaction
{
  RPMOSTreeTransactionSkeleton parent;
  TransactionPrivate *priv;
};

struct _TransactionClass
{
  RPMOSTreeTransactionSkeletonClass parent_class;

  gboolean      (*execute)                 (Transaction *transaction,
                                            GCancellable *cancellable,
                                            GError **error);
};

GType           transaction_get_type            (void) G_GNUC_CONST;
OstreeSysroot * transaction_get_sysroot         (Transaction *transaction);
GDBusMethodInvocation *
                transaction_get_invocation      (Transaction *transaction);
const char *    transaction_get_client_address  (Transaction *transaction);
void            transaction_emit_message_printf (Transaction *transaction,
                                                 const char *format,
                                                 ...) G_GNUC_PRINTF (2, 3);
void            transaction_connect_download_progress
                                                (Transaction *transaction,
                                                 OstreeAsyncProgress *progress);
void            transaction_connect_signature_progress
                                                (Transaction *transaction,
                                                 OstreeRepo *repo);
