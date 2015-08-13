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
  GCancellable *cancellable;

  /* Locked for the duration of the transaction. */
  OstreeSysroot *sysroot;

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
  PROP_SYSROOT
};

enum {
  CANCELLED,
  FINISHED,
  OWNER_VANISHED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static void transaction_initable_iface_init (GInitableIface *iface);
static void transaction_dbus_iface_init (RPMOSTreeTransactionIface *iface);

G_DEFINE_TYPE_WITH_CODE (Transaction, transaction,
                         RPMOSTREE_TYPE_TRANSACTION_SKELETON,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                transaction_initable_iface_init)
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_TRANSACTION,
                                                transaction_dbus_iface_init))

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
      case PROP_SYSROOT:
        transaction->sysroot = g_value_dup_object (value);
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
      case PROP_SYSROOT:
        g_value_set_object (value, transaction->sysroot);
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

  if (transaction->sysroot != NULL)
    ostree_sysroot_unlock (transaction->sysroot);

  g_clear_object (&transaction->cancellable);
  g_clear_object (&transaction->sysroot);

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
transaction_initable_init (GInitable *initable,
                           GCancellable *cancellable,
                           GError **error)
{
  Transaction *transaction = TRANSACTION (initable);
  gboolean ret = FALSE;

  if (G_IS_CANCELLABLE (cancellable))
    transaction->cancellable = g_object_ref (cancellable);

  if (transaction->sysroot != NULL)
    {
      gboolean lock_acquired = FALSE;

      if (!ostree_sysroot_try_lock (transaction->sysroot,
                                    &lock_acquired, error))
        goto out;

      if (!lock_acquired)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                               "System transaction in progress");
          goto out;
        }
    }

  ret = TRUE;

out:
  return ret;
}

static gboolean
transaction_handle_cancel (RPMOSTreeTransaction *transaction,
                           GDBusMethodInvocation *invocation)
{
  Transaction *real_transaction = TRANSACTION (transaction);

  if (real_transaction->cancellable == NULL)
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
      g_cancellable_cancel (real_transaction->cancellable);
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
                                   PROP_SYSROOT,
                                   g_param_spec_object ("sysroot",
                                                        "Sysroot",
                                                        "An OstreeSysroot instance",
                                                        OSTREE_TYPE_SYSROOT,
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
transaction_initable_iface_init (GInitableIface *iface)
{
  iface->init = transaction_initable_init;
}

static void
transaction_dbus_iface_init (RPMOSTreeTransactionIface *iface)
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
                 OstreeSysroot *sysroot,
                 GCancellable *cancellable,
                 GError **error)
{
  Transaction *transaction;
  GDBusConnection *connection;
  const char *method_name;
  const char *sender;

  /* sysroot is optional */
  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);

  method_name = g_dbus_method_invocation_get_method_name (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  transaction = g_initable_new (TYPE_TRANSACTION,
                                cancellable, error,
                                "sysroot", sysroot,
                                "method-name", method_name,
                                "owner", sender,
                                "active", TRUE,
                                NULL);

  if (transaction == NULL)
    goto out;

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

out:
  return (RPMOSTreeTransaction *) transaction;
}

OstreeSysroot *
transaction_get_sysroot (RPMOSTreeTransaction *transaction)
{
  Transaction *real_transaction;

  g_return_val_if_fail (RPMOSTREE_IS_TRANSACTION (transaction), NULL);

  real_transaction = TRANSACTION (transaction);

  return real_transaction->sysroot;
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
