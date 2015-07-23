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
#include "ostree.h"

#include <libglnx.h>

#include "transaction.h"
#include "errors.h"

typedef struct _TransactionClass TransactionClass;

struct _Transaction
{
  RPMOSTreeTransactionSkeleton parent;
  GCancellable *method_cancellable;

  gboolean success;
  char *message;

  guint watch_id;
};

struct _TransactionClass
{
  RPMOSTreeTransactionSkeletonClass parent_class;
};

enum {
  PROP_0,
  PROP_CANCELLABLE
};

enum {
  CANCELLED,
  FINISHED,
  OWNER_VANISHED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static void transaction_iface_init (RPMOSTreeTransactionIface *iface);

G_DEFINE_TYPE_WITH_CODE (Transaction, transaction,
                         RPMOSTREE_TYPE_TRANSACTION_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_TRANSACTION,
                                                transaction_iface_init))

static gboolean
transaction_check_sender_is_owner (RPMOSTreeTransaction *transaction,
                                   GDBusMethodInvocation *invocation)
{
  const char *sender, *owner;

  owner = rpmostree_transaction_get_owner (transaction);
  sender = g_dbus_method_invocation_get_sender (invocation);

  return (g_strcmp0 (owner, sender) == 0);
}

static void
transaction_owner_vanished_cb (GDBusConnection *connection,
                               const char *name,
                               gpointer user_data)
{
  Transaction *transaction = TRANSACTION (user_data);

  if (transaction->watch_id > 0)
    {
      g_bus_unwatch_name (transaction->watch_id);
      transaction->watch_id = 0;

      /* Emit the signal AFTER unwatching the bus name, since this
       * may finalize the transaction and invalidate the watch_id. */
      g_signal_emit (transaction, signals[OWNER_VANISHED], 0);
    }
}

static void
transaction_progress_changed_cb (OstreeAsyncProgress *progress,
                                 RPMOSTreeTransaction *transaction)
{
  guint64 start_time = ostree_async_progress_get_uint64 (progress, "start-time");
  guint64 elapsed_secs = 0;

  guint outstanding_fetches = ostree_async_progress_get_uint (progress, "outstanding-fetches");
  guint outstanding_writes = ostree_async_progress_get_uint (progress, "outstanding-writes");

  guint n_scanned_metadata = ostree_async_progress_get_uint (progress, "scanned-metadata");
  guint metadata_fetched = ostree_async_progress_get_uint (progress, "metadata-fetched");
  guint outstanding_metadata_fetches = ostree_async_progress_get_uint (progress, "outstanding-metadata-fetches");

  guint total_delta_parts = ostree_async_progress_get_uint (progress, "total-delta-parts");
  guint fetched_delta_parts = ostree_async_progress_get_uint (progress, "fetched-delta-parts");
  guint total_delta_superblocks = ostree_async_progress_get_uint (progress, "total-delta-superblocks");
  guint64 total_delta_part_size = ostree_async_progress_get_uint64 (progress, "total-delta-part-size");

  guint fetched = ostree_async_progress_get_uint (progress, "fetched");
  guint requested = ostree_async_progress_get_uint (progress, "requested");

  guint64 bytes_sec = 0;
  guint64 bytes_transferred = ostree_async_progress_get_uint64 (progress, "bytes-transferred");

  GVariant *arg_time;
  GVariant *arg_outstanding;
  GVariant *arg_metadata;
  GVariant *arg_delta;
  GVariant *arg_content;
  GVariant *arg_transfer;

  g_autofree gchar *status;

  /* If there is a status that is all we output */
  status = ostree_async_progress_get_status (progress);
  if (status) {
    rpmostree_transaction_emit_message (transaction, g_strdup (status));
    return;
  }

  if (start_time)
    {
      guint64 elapsed_secs = (g_get_monotonic_time () - start_time) / G_USEC_PER_SEC;
      if (elapsed_secs)
        bytes_sec = bytes_transferred / elapsed_secs;
    }

  arg_time = g_variant_new ("(tt)",
                            start_time,
                            elapsed_secs);

  arg_outstanding = g_variant_new ("(uu)",
                                   outstanding_fetches,
                                   outstanding_writes);

  arg_metadata = g_variant_new ("(uuu)",
                                n_scanned_metadata,
                                metadata_fetched,
                                outstanding_metadata_fetches);

  arg_delta = g_variant_new ("(uuut)",
                             total_delta_parts,
                             fetched_delta_parts,
                             total_delta_superblocks,
                             total_delta_part_size);

  arg_content = g_variant_new ("(uu)",
                               fetched,
                               requested);

  arg_transfer = g_variant_new ("(tt)",
                                bytes_transferred,
                                bytes_sec);

  /* This sinks the floating GVariant refs (I think...). */
  rpmostree_transaction_emit_download_progress (transaction,
                                                arg_time,
                                                arg_outstanding,
                                                arg_metadata,
                                                arg_delta,
                                                arg_content,
                                                arg_transfer);
}

static void
transaction_gpg_verify_result_cb (OstreeRepo *repo,
                                  const char *checksum,
                                  OstreeGpgVerifyResult *result,
                                  RPMOSTreeTransaction *transaction)
{
  guint n, i;
  GVariantBuilder builder;

  if (!rpmostree_transaction_get_active (transaction))
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));

  n = ostree_gpg_verify_result_count_all (result);

  for (i = 0; i < n; i++)
    {
      g_variant_builder_add (&builder, "v",
      ostree_gpg_verify_result_get_all (result, i));
    }

  rpmostree_transaction_emit_signature_progress (transaction,
                                                 g_variant_builder_end (&builder),
                                                 g_strdup (checksum));
}

static void
transaction_set_property (GObject *object,
                          guint property_id,
                          const GValue *value,
                          GParamSpec *pspec)
{
  Transaction *transaction = TRANSACTION (object);

  switch (property_id)
    {
      case PROP_CANCELLABLE:
        transaction->method_cancellable = g_value_dup_object (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
transaction_get_property (GObject *object,
                          guint property_id,
                          GValue *value,
                          GParamSpec *pspec)
{
  Transaction *transaction = TRANSACTION (object);

  switch (property_id)
    {
      case PROP_CANCELLABLE:
        g_value_set_object (value, transaction->method_cancellable);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
transaction_dispose (GObject *object)
{
  Transaction *transaction = TRANSACTION (object);

  g_clear_object (&transaction->method_cancellable);

  G_OBJECT_CLASS (transaction_parent_class)->dispose (object);
}

static void
transaction_finalize (GObject *object)
{
  Transaction *transaction = TRANSACTION (object);

  g_free (transaction->message);

  if (transaction->watch_id > 0)
    g_bus_unwatch_name (transaction->watch_id);

  G_OBJECT_CLASS (transaction_parent_class)->finalize (object);
}

static gboolean
transaction_handle_cancel (RPMOSTreeTransaction *transaction,
                           GDBusMethodInvocation *invocation)
{
  Transaction *real_transaction = TRANSACTION (transaction);

  if (real_transaction->method_cancellable == NULL)
    return FALSE;

  if (!transaction_check_sender_is_owner (transaction, invocation))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             RPM_OSTREED_ERROR,
                                             RPM_OSTREED_ERROR_FAILED,
                                             "You are not allowed to cancel this transaction");
    }
  else
    {
      g_cancellable_cancel (real_transaction->method_cancellable);
      g_signal_emit (transaction, signals[CANCELLED], 0);
      rpmostree_transaction_complete_cancel (transaction, invocation);
    }

  return TRUE;
}

static gboolean
transaction_handle_finish (RPMOSTreeTransaction *transaction,
                           GDBusMethodInvocation *invocation)
{
  Transaction *real_transaction = TRANSACTION (transaction);

  if (!transaction_check_sender_is_owner (transaction, invocation))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             RPM_OSTREED_ERROR,
                                             RPM_OSTREED_ERROR_FAILED,
                                             "You are not allowed to finish this transaction");
    }
  else if (rpmostree_transaction_get_active (transaction))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             RPM_OSTREED_ERROR,
                                             RPM_OSTREED_ERROR_FAILED,
                                             "Transaction is still active");
    }
  else
    {
      g_signal_emit (transaction, signals[FINISHED], 0);
      rpmostree_transaction_complete_finish (transaction, invocation,
                                             real_transaction->success,
                                             real_transaction->message ?
                                             real_transaction->message : "");
    }

  return TRUE;
}

static void
transaction_class_init (TransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->set_property = transaction_set_property;
  object_class->get_property = transaction_get_property;
  object_class->dispose = transaction_dispose;
  object_class->finalize = transaction_finalize;

  g_object_class_install_property (object_class,
                                   PROP_CANCELLABLE,
                                   g_param_spec_object ("cancellable",
                                                        NULL,
                                                        NULL,
                                                        G_TYPE_CANCELLABLE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  signals[CANCELLED] = g_signal_new ("cancelled",
                                     TYPE_TRANSACTION,
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);

  signals[FINISHED] = g_signal_new ("finished",
                                    TYPE_TRANSACTION,
                                    G_SIGNAL_RUN_LAST,
                                    0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 0);

  signals[OWNER_VANISHED] = g_signal_new ("owner-vanished",
                                          TYPE_TRANSACTION,
                                          G_SIGNAL_RUN_LAST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
}

static void
transaction_iface_init (RPMOSTreeTransactionIface *iface)
{
  iface->handle_cancel = transaction_handle_cancel;
  iface->handle_finish = transaction_handle_finish;
}

static void
transaction_init (Transaction *transaction)
{
}

RPMOSTreeTransaction *
transaction_new (GDBusMethodInvocation *invocation,
                 GCancellable *method_cancellable)
{
  Transaction *transaction;
  GDBusConnection *connection;
  const char *method_name;
  const char *sender;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);

  method_name = g_dbus_method_invocation_get_method_name (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  transaction = g_object_new (TYPE_TRANSACTION,
                              "cancellable", method_cancellable,
                              "method-name", method_name,
                              "owner", sender,
                              "active", TRUE,
                              NULL);

  /* XXX Would be handy if GDBusInterfaceSkeleton had an export()
   *     class method so subclasses can know when a GDBusConnection
   *     becomes available.  Alas, just set up the sender bus name
   *     watching here using the GDBusMethodInvocation's connection. */

  connection = g_dbus_method_invocation_get_connection (invocation);

  transaction->watch_id = g_bus_watch_name_on_connection (connection,
                                                          sender,
                                                          G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                          NULL,
                                                          transaction_owner_vanished_cb,
                                                          transaction,
                                                          NULL);

  return RPMOSTREE_TRANSACTION (transaction);
}

void
transaction_emit_message_printf (RPMOSTreeTransaction *transaction,
                                 const char *format,
                                 ...)
{
  g_autofree char *message = NULL;
  va_list args;

  g_return_if_fail (RPMOSTREE_IS_TRANSACTION (transaction));
  g_return_if_fail (format != NULL);

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  rpmostree_transaction_emit_message (transaction, message);
}

void
transaction_done (RPMOSTreeTransaction *transaction,
                  gboolean success,
                  const char *message)
{
  Transaction *real_transaction;

  g_return_if_fail (RPMOSTREE_IS_TRANSACTION (transaction));

  if (message == NULL)
    message = "";

  real_transaction = TRANSACTION (transaction);
  real_transaction->success = success;
  real_transaction->message = g_strdup (message);

  rpmostree_transaction_set_active (transaction, FALSE);
}

void
transaction_connect_download_progress (RPMOSTreeTransaction *transaction,
                                       OstreeAsyncProgress *progress)
{
  g_return_if_fail (RPMOSTREE_IS_TRANSACTION (transaction));
  g_return_if_fail (OSTREE_IS_ASYNC_PROGRESS (progress));

  g_signal_connect_object (progress,
                           "changed",
                           G_CALLBACK (transaction_progress_changed_cb),
                           transaction, 0);
}

void
transaction_connect_signature_progress (RPMOSTreeTransaction *transaction,
                                        OstreeRepo *repo)
{
  g_return_if_fail (RPMOSTREE_IS_TRANSACTION (transaction));
  g_return_if_fail (OSTREE_REPO (repo));

  g_signal_connect_object (repo, "gpg-verify-result",
                           G_CALLBACK (transaction_gpg_verify_result_cb),
                           transaction, 0);
}
