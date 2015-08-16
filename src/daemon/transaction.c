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

struct _TransactionPrivate {
  GDBusMethodInvocation *invocation;
  GCancellable *cancellable;

  /* Locked for the duration of the transaction. */
  OstreeSysroot *sysroot;

  gboolean started;
  gboolean success;
  char *message;

  guint watch_id;
};

enum {
  PROP_0,
  PROP_INVOCATION,
  PROP_SYSROOT
};

enum {
  START,
  CANCELLED,
  FINISHED,
  OWNER_VANISHED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static void transaction_initable_iface_init (GInitableIface *iface);
static void transaction_dbus_iface_init (RPMOSTreeTransactionIface *iface);

/* XXX I tried using G_ADD_PRIVATE here, but was getting memory corruption
 *     on the 2nd instance and valgrind was going crazy with invalid reads
 *     and writes.  So I'm falling back to the allegedly deprecated method
 *     and deferring further investigation. */
G_DEFINE_TYPE_WITH_CODE (Transaction, transaction,
                         RPMOSTREE_TYPE_TRANSACTION_SKELETON,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                transaction_initable_iface_init)
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_TRANSACTION,
                                                transaction_dbus_iface_init))

/* XXX This is lame but it's meant to keep it simple to
 *     transition to transaction_get_instance_private(). */
static TransactionPrivate *
transaction_get_private (Transaction *self)
{
  return self->priv;
}

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
  TransactionPrivate *priv = transaction_get_private (transaction);

  if (priv->watch_id > 0)
    {
      g_bus_unwatch_name (priv->watch_id);
      priv->watch_id = 0;

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
  Transaction *self = TRANSACTION (object);
  TransactionPrivate *priv = transaction_get_private (self);

  switch (property_id)
    {
      case PROP_INVOCATION:
        priv->invocation = g_value_dup_object (value);
      case PROP_SYSROOT:
        priv->sysroot = g_value_dup_object (value);
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
  Transaction *self = TRANSACTION (object);
  TransactionPrivate *priv = transaction_get_private (self);

  switch (property_id)
    {
      case PROP_INVOCATION:
        g_value_set_object (value, priv->invocation);
        break;
      case PROP_SYSROOT:
        g_value_set_object (value, priv->sysroot);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
transaction_dispose (GObject *object)
{
  Transaction *self = TRANSACTION (object);
  TransactionPrivate *priv = transaction_get_private (self);

  if (priv->sysroot != NULL)
    ostree_sysroot_unlock (priv->sysroot);

  g_clear_object (&priv->invocation);
  g_clear_object (&priv->cancellable);
  g_clear_object (&priv->sysroot);

  G_OBJECT_CLASS (transaction_parent_class)->dispose (object);
}

static void
transaction_finalize (GObject *object)
{
  Transaction *self = TRANSACTION (object);
  TransactionPrivate *priv = transaction_get_private (self);

  g_free (priv->message);

  if (priv->watch_id > 0)
    g_bus_unwatch_name (priv->watch_id);

  G_OBJECT_CLASS (transaction_parent_class)->finalize (object);
}

static void
transaction_constructed (GObject *object)
{
  Transaction *self = TRANSACTION (object);
  TransactionPrivate *priv = transaction_get_private (self);

  G_OBJECT_CLASS (transaction_parent_class)->constructed (object);

  if (priv->invocation != NULL)
    {
      GDBusConnection *connection;
      const char *method_name;
      const char *sender;

      connection = g_dbus_method_invocation_get_connection (priv->invocation);
      method_name = g_dbus_method_invocation_get_method_name (priv->invocation);
      sender = g_dbus_method_invocation_get_sender (priv->invocation);

      /* Initialize D-Bus properties. */
      g_object_set (self,
                    "method-name", method_name,
                    "owner", sender,
                    "active", TRUE,
                    NULL);

      priv->watch_id = g_bus_watch_name_on_connection (connection,
                                                       sender,
                                                       G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       NULL,
                                                       transaction_owner_vanished_cb,
                                                       self,
                                                       NULL);
    }
}

static gboolean
transaction_initable_init (GInitable *initable,
                           GCancellable *cancellable,
                           GError **error)
{
  Transaction *self = TRANSACTION (initable);
  TransactionPrivate *priv = transaction_get_private (self);
  gboolean ret = FALSE;

  if (G_IS_CANCELLABLE (cancellable))
    priv->cancellable = g_object_ref (cancellable);

  if (priv->sysroot != NULL)
    {
      gboolean lock_acquired = FALSE;

      if (!ostree_sysroot_try_lock (priv->sysroot, &lock_acquired, error))
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
  Transaction *self = TRANSACTION (transaction);
  TransactionPrivate *priv = transaction_get_private (self);

  if (priv->cancellable == NULL)
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
      g_cancellable_cancel (priv->cancellable);
      g_signal_emit (transaction, signals[CANCELLED], 0);
      rpmostree_transaction_complete_cancel (transaction, invocation);
    }

  return TRUE;
}

static gboolean
transaction_handle_start (RPMOSTreeTransaction *transaction,
                          GDBusMethodInvocation *invocation)
{
  Transaction *self = TRANSACTION (transaction);
  TransactionPrivate *priv = transaction_get_private (self);

  if (priv->started)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             RPM_OSTREED_ERROR,
                                             RPM_OSTREED_ERROR_FAILED,
                                             "Transaction has already started");
    }
  else
    {
      priv->started = TRUE;

      /* FIXME Subclassing would be better than this. */
      g_signal_emit (transaction, signals[START], 0);

      rpmostree_transaction_complete_start (transaction, invocation);
    }

  return TRUE;
}

static gboolean
transaction_handle_finish (RPMOSTreeTransaction *transaction,
                           GDBusMethodInvocation *invocation)
{
  Transaction *self = TRANSACTION (transaction);
  TransactionPrivate *priv = transaction_get_private (self);

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
                                             priv->success,
                                             priv->message ?
                                             priv->message : "");
    }

  return TRUE;
}

static void
transaction_class_init (TransactionClass *class)
{
  GObjectClass *object_class;

  g_type_class_add_private (class, sizeof (TransactionPrivate));

  object_class = G_OBJECT_CLASS (class);
  object_class->set_property = transaction_set_property;
  object_class->get_property = transaction_get_property;
  object_class->dispose = transaction_dispose;
  object_class->finalize = transaction_finalize;
  object_class->constructed = transaction_constructed;

  g_object_class_install_property (object_class,
                                   PROP_INVOCATION,
                                   g_param_spec_object ("invocation",
                                                        "Invocation",
                                                        "D-Bus method invocation",
                                                        G_TYPE_DBUS_METHOD_INVOCATION,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class,
                                   PROP_SYSROOT,
                                   g_param_spec_object ("sysroot",
                                                        "Sysroot",
                                                        "An OstreeSysroot instance",
                                                        OSTREE_TYPE_SYSROOT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  signals[START] = g_signal_new ("start",
                                 TYPE_TRANSACTION,
                                 G_SIGNAL_RUN_LAST,
                                 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);

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
  iface->handle_start  = transaction_handle_start;
  iface->handle_finish = transaction_handle_finish;
}

static void
transaction_init (Transaction *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                            TYPE_TRANSACTION,
                                            TransactionPrivate);
}

RPMOSTreeTransaction *
transaction_new (GDBusMethodInvocation *invocation,
                 OstreeSysroot *sysroot,
                 GCancellable *cancellable,
                 GError **error)
{
  /* sysroot is optional */
  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);

  return g_initable_new (TYPE_TRANSACTION,
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot", sysroot,
                         NULL);
}

OstreeSysroot *
transaction_get_sysroot (RPMOSTreeTransaction *transaction)
{
  Transaction *self;
  TransactionPrivate *priv;

  g_return_val_if_fail (RPMOSTREE_IS_TRANSACTION (transaction), NULL);

  self = TRANSACTION (transaction);
  priv = transaction_get_private (self);

  return priv->sysroot;
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
  Transaction *self;
  TransactionPrivate *priv;

  g_return_if_fail (RPMOSTREE_IS_TRANSACTION (transaction));

  if (message == NULL)
    message = "";

  self = TRANSACTION (transaction);
  priv = transaction_get_private (self);

  priv->success = success;
  priv->message = g_strdup (message);

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
