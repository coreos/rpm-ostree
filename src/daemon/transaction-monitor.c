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

#include "config.h"

#include "transaction-monitor.h"

#include <libglnx.h>

#include "daemon.h"
#include "transaction.h"

typedef struct _TransactionMonitorClass TransactionMonitorClass;

struct _TransactionMonitor
{
  GObjectClass parent;

  /* The head of the queue is the active transaction. */
  GQueue *transactions;
};

struct _TransactionMonitorClass
{
  GObjectClass parent_class;
};

enum {
  PROP_0,
  PROP_ACTIVE_TRANSACTION
};

G_DEFINE_TYPE (TransactionMonitor, transaction_monitor, G_TYPE_OBJECT)

static void
transaction_monitor_remove_transaction (TransactionMonitor *monitor,
                                        RPMOSTreeTransaction *transaction)
{
  GList *link;

  link = g_queue_find (monitor->transactions, transaction);

  if (link != NULL)
    {
      GList *head;
      gboolean need_notify;

      /* The head of the queue is the active transaction. */
      head = g_queue_peek_head_link (monitor->transactions);
      need_notify = (link == head);

      g_object_unref (link->data);
      g_queue_delete_link (monitor->transactions, link);

      if (need_notify)
        {
          /* Issue a notification so property bindings get updated. */
          g_object_notify (G_OBJECT (monitor), "active-transaction");
        }
    }
}

static void
transaction_monitor_notify_active_cb (RPMOSTreeTransaction *transaction,
                                      GParamSpec *pspec,
                                      TransactionMonitor *monitor)
{
  GList *head, *link;

  /* The head of the queue is the active transaction. */
  head = g_queue_peek_head_link (monitor->transactions);
  link = g_queue_find (monitor->transactions, transaction);

  if (link == head)
    {
      /* Issue a notification so property bindings get updated. */
      g_object_notify (G_OBJECT (monitor), "active-transaction");
    }
}

static void
transaction_monitor_cancelled_cb (RPMOSTreeTransaction *transaction,
                                  TransactionMonitor *monitor)
{
  transaction_monitor_remove_transaction (monitor, transaction);
}

static void
transaction_monitor_closed_cb (RPMOSTreeTransaction *transaction,
                               TransactionMonitor *monitor)
{
  transaction_monitor_remove_transaction (monitor, transaction);
}

static void
transaction_monitor_owner_vanished_cb (RPMOSTreeTransaction *transaction,
                                       TransactionMonitor *monitor)
{
  transaction_monitor_remove_transaction (monitor, transaction);
}

static void
transaction_monitor_get_property (GObject *object,
                                  guint property_id,
                                  GValue *value,
                                  GParamSpec *pspec)
{
  TransactionMonitor *monitor = TRANSACTION_MONITOR (object);
  gpointer v_object;

  switch (property_id)
    {
      case PROP_ACTIVE_TRANSACTION:
        v_object = transaction_monitor_ref_active_transaction (monitor);
        g_value_take_object (value, v_object);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
transaction_monitor_dispose (GObject *object)
{
  TransactionMonitor *monitor = TRANSACTION_MONITOR (object);

  g_queue_free_full (monitor->transactions, (GDestroyNotify) g_object_unref);

  G_OBJECT_CLASS (transaction_monitor_parent_class)->dispose (object);
}

static void
transaction_monitor_class_init (TransactionMonitorClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->get_property = transaction_monitor_get_property;
  object_class->dispose = transaction_monitor_dispose;

  g_object_class_install_property (object_class,
                                   PROP_ACTIVE_TRANSACTION,
                                   g_param_spec_object ("active-transaction",
                                                        NULL,
                                                        NULL,
                                                        TYPE_TRANSACTION,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
transaction_monitor_init (TransactionMonitor *monitor)
{
  monitor->transactions = g_queue_new ();
}

TransactionMonitor *
transaction_monitor_new (void)
{
  return g_object_new (TYPE_TRANSACTION_MONITOR, NULL);
}

void
transaction_monitor_add (TransactionMonitor *monitor,
                         Transaction *transaction)
{
  g_return_if_fail (IS_TRANSACTION_MONITOR (monitor));
  g_return_if_fail (IS_TRANSACTION (transaction));

  g_signal_connect_object (transaction, "notify::active",
                           G_CALLBACK (transaction_monitor_notify_active_cb),
                           monitor, 0);

  g_signal_connect_object (transaction, "cancelled",
                           G_CALLBACK (transaction_monitor_cancelled_cb),
                           monitor, 0);

  g_signal_connect_object (transaction, "closed",
                           G_CALLBACK (transaction_monitor_closed_cb),
                           monitor, 0);

  g_signal_connect_object (transaction, "owner-vanished",
                           G_CALLBACK (transaction_monitor_owner_vanished_cb),
                           monitor, 0);

  g_queue_push_head (monitor->transactions, g_object_ref (transaction));
  g_object_notify (G_OBJECT (monitor), "active-transaction");
}

Transaction *
transaction_monitor_ref_active_transaction (TransactionMonitor *monitor)
{
  Transaction *transaction;

  g_return_val_if_fail (IS_TRANSACTION_MONITOR (monitor), NULL);

  /* The head of the queue is the active transaction. */
  transaction = g_queue_peek_head (monitor->transactions);

  if (transaction != NULL)
    {
      /* An "inactive" transaction is waiting to be Finish()'ed by its
       * client, but it doesn't block other transactions from starting. */
      if (rpmostree_transaction_get_active (RPMOSTREE_TRANSACTION (transaction)))
        g_object_ref (transaction);
      else
        transaction = NULL;
    }

  return transaction;
}
