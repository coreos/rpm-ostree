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

#include "ostree.h"
#include "rpmostreed-types.h"

#define RPMOSTREED_TYPE_TRANSACTION_MONITOR   (rpmostreed_transaction_monitor_get_type ())
#define RPMOSTREED_TRANSACTION_MONITOR(o)     (G_TYPE_CHECK_INSTANCE_CAST ((o), RPMOSTREED_TYPE_TRANSACTION_MONITOR, RpmostreedTransactionMonitor))
#define RPMOSTREED_IS_TRANSACTION_MONITOR(o)  (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPMOSTREED_TYPE_TRANSACTION_MONITOR))

GType           rpmostreed_transaction_monitor_get_type    (void) G_GNUC_CONST;
RpmostreedTransactionMonitor *
                rpmostreed_transaction_monitor_new         (void);
void            rpmostreed_transaction_monitor_add         (RpmostreedTransactionMonitor *monitor,
                                                            RpmostreedTransaction *transaction);
RpmostreedTransaction *
                rpmostreed_transaction_monitor_ref_active_transaction
                                                           (RpmostreedTransactionMonitor *monitor);
