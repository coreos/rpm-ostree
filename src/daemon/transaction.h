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

#define TYPE_TRANSACTION   (transaction_get_type ())
#define TRANSACTION(o)     (G_TYPE_CHECK_INSTANCE_CAST ((o), TYPE_TRANSACTION, Transaction))
#define IS_TRANSACTION(o)  (G_TYPE_CHECK_INSTANCE_TYPE ((o), TYPE_TRANSACTION))

GType           transaction_get_type            (void) G_GNUC_CONST;
RPMOSTreeTransaction *
                transaction_new                 (GDBusMethodInvocation *invocation,
                                                 GCancellable *method_cancellable);
void            transaction_done                (RPMOSTreeTransaction *transaction,
                                                 gboolean success,
                                                 const char *message);
void            transaction_emit_message_printf (RPMOSTreeTransaction *transaction,
                                                 const char *format,
                                                 ...) G_GNUC_PRINTF (2, 3);
void            transaction_connect_download_progress
                                                (RPMOSTreeTransaction *transaction,
                                                 OstreeAsyncProgress *progress);
void            transaction_connect_signature_progress
                                                (RPMOSTreeTransaction *transaction,
                                                 OstreeRepo *repo);
