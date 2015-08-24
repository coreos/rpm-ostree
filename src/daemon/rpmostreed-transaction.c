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

#include "rpmostreed-transaction.h"
#include "rpmostreed-errors.h"
#include "rpmostreed-sysroot.h"

struct _RpmostreedTransactionPrivate {
  GDBusMethodInvocation *invocation;
  GCancellable *cancellable;

  /* Locked for the duration of the transaction. */
  OstreeSysroot *sysroot;

  GDBusServer *server;
  GDBusConnection *peer_connection;

  gboolean started;

  guint watch_id;
};

enum {
  PROP_0,
  PROP_INVOCATION,
  PROP_SYSROOT
};

enum {
  CANCELLED,
  CLOSED,
  OWNER_VANISHED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static void rpmostreed_transaction_initable_iface_init (GInitableIface *iface);
static void rpmostreed_transaction_dbus_iface_init (RPMOSTreeTransactionIface *iface);

/* XXX I tried using G_ADD_PRIVATE here, but was getting memory corruption
 *     on the 2nd instance and valgrind was going crazy with invalid reads
 *     and writes.  So I'm falling back to the allegedly deprecated method
 *     and deferring further investigation. */
G_DEFINE_ABSTRACT_TYPE_WITH_CODE (RpmostreedTransaction,
                                  rpmostreed_transaction,
                                  RPMOSTREE_TYPE_TRANSACTION_SKELETON,
                                  G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                         rpmostreed_transaction_initable_iface_init)
                                  G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_TRANSACTION,
                                                         rpmostreed_transaction_dbus_iface_init))

/* XXX This is lame but it's meant to keep it simple to
 *     transition to transaction_get_instance_private(). */
static RpmostreedTransactionPrivate *
rpmostreed_transaction_get_private (RpmostreedTransaction *self)
{
  return self->priv;
}

static void
transaction_connection_closed_cb (GDBusConnection *connection,
                                  gboolean remote_peer_vanished,
                                  GError *error,
                                  RpmostreedTransaction *self)
{
  g_debug ("%s (%p): Client disconnected",
           G_OBJECT_TYPE_NAME (self), self);

  g_signal_emit (self, signals[CLOSED], 0);
}

static gboolean
transaction_new_connection_cb (GDBusServer *server,
                               GDBusConnection *connection,
                               RpmostreedTransaction *self)
{
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);
  GError *local_error = NULL;

  if (priv->peer_connection != NULL)
    return FALSE;

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self),
                                    connection, "/", &local_error);

  if (local_error != NULL)
    {
      g_critical ("Failed to export interface: %s", local_error->message);
      g_clear_error (&local_error);
      return FALSE;
    }

  g_signal_connect_object (connection,
                           "closed",
                           G_CALLBACK (transaction_connection_closed_cb),
                           self, 0);

  priv->peer_connection = g_object_ref (connection);

  g_debug ("%s (%p): Client connected",
           G_OBJECT_TYPE_NAME (self), self);

  return TRUE;
}

static void
transaction_owner_vanished_cb (GDBusConnection *connection,
                               const char *name,
                               gpointer user_data)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (user_data);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  if (priv->watch_id > 0)
    {
      g_bus_unwatch_name (priv->watch_id);
      priv->watch_id = 0;

      /* Emit the signal AFTER unwatching the bus name, since this
       * may finalize the transaction and invalidate the watch_id. */
      g_signal_emit (self, signals[OWNER_VANISHED], 0);
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
transaction_execute_thread (GTask *task,
                            gpointer source_object,
                            gpointer task_data,
                            GCancellable *cancellable)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (source_object);
  RpmostreedTransactionClass *class = RPMOSTREED_TRANSACTION_GET_CLASS (self);
  gboolean success = TRUE;
  GError *local_error = NULL;

  if (class->execute != NULL)
    success = class->execute (self, cancellable, &local_error);

  if (local_error != NULL)
    g_task_return_error (task, local_error);
  else
    g_task_return_boolean (task, success);
}

static void
transaction_execute_done_cb (GObject *source_object,
                             GAsyncResult *result,
                             gpointer user_data)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (source_object);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);
  const char *error_message = NULL;
  gboolean success;
  GError *local_error = NULL;

  success = g_task_propagate_boolean (G_TASK (result), &local_error);

  /* Sanity check */
  g_warn_if_fail ((success && local_error == NULL) ||
                  (!success && local_error != NULL));

  if (local_error != NULL)
    error_message = local_error->message;

  if (error_message == NULL)
    error_message = "";

  if (success && priv->sysroot != NULL)
    rpmostreed_sysroot_emit_update (rpmostreed_sysroot_get (), priv->sysroot);

  g_debug ("%s (%p): Finished%s%s%s",
           G_OBJECT_TYPE_NAME (self), self,
           success ? "" : " (error: ",
           success ? "" : error_message,
           success ? "" : ")");

  rpmostree_transaction_set_active (RPMOSTREE_TRANSACTION (self), FALSE);

  rpmostree_transaction_emit_finished (RPMOSTREE_TRANSACTION (self),
                                       success, error_message);
}

static void
transaction_set_property (GObject *object,
                          guint property_id,
                          const GValue *value,
                          GParamSpec *pspec)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (object);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

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
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (object);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

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
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (object);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  if (priv->sysroot != NULL)
    ostree_sysroot_unlock (priv->sysroot);

  g_clear_object (&priv->invocation);
  g_clear_object (&priv->cancellable);
  g_clear_object (&priv->sysroot);
  g_clear_object (&priv->server);
  g_clear_object (&priv->peer_connection);

  G_OBJECT_CLASS (rpmostreed_transaction_parent_class)->dispose (object);
}

static void
transaction_finalize (GObject *object)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (object);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  g_debug ("%s (%p): Finalized", G_OBJECT_TYPE_NAME (self), self);

  if (priv->watch_id > 0)
    g_bus_unwatch_name (priv->watch_id);

  G_OBJECT_CLASS (rpmostreed_transaction_parent_class)->finalize (object);
}

static void
transaction_constructed (GObject *object)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (object);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  G_OBJECT_CLASS (rpmostreed_transaction_parent_class)->constructed (object);

  if (priv->invocation != NULL)
    {
      GDBusConnection *connection;
      const char *sender;

      connection = g_dbus_method_invocation_get_connection (priv->invocation);
      sender = g_dbus_method_invocation_get_sender (priv->invocation);

      /* Initialize D-Bus properties. */
      rpmostree_transaction_set_active (RPMOSTREE_TRANSACTION (self), TRUE);

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
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (initable);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);
  g_autofree char *guid = NULL;
  gboolean ret = FALSE;

  if (G_IS_CANCELLABLE (cancellable))
    priv->cancellable = g_object_ref (cancellable);

  /* Set up a private D-Bus server over which to emit
   * progress and informational messages to the caller. */

  guid = g_dbus_generate_guid ();

  priv->server = g_dbus_server_new_sync ("unix:tmpdir=/tmp/rpm-ostree",
                                         G_DBUS_SERVER_FLAGS_NONE,
                                         guid,
                                         NULL,
                                         cancellable,
                                         error);

  if (priv->server == NULL)
    goto out;

  g_signal_connect_object (priv->server,
                           "new-connection",
                           G_CALLBACK (transaction_new_connection_cb),
                           self, 0);

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

  g_dbus_server_start (priv->server);

  g_debug ("%s (%p): Initialized, listening on %s",
           G_OBJECT_TYPE_NAME (self), self,
           rpmostreed_transaction_get_client_address (self));

  ret = TRUE;

out:
  return ret;
}

static gboolean
transaction_handle_cancel (RPMOSTreeTransaction *transaction,
                           GDBusMethodInvocation *invocation)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (transaction);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  if (priv->cancellable == NULL)
    return FALSE;

  g_debug ("%s (%p): Cancelled", G_OBJECT_TYPE_NAME (self), self);

  g_cancellable_cancel (priv->cancellable);
  g_signal_emit (transaction, signals[CANCELLED], 0);
  rpmostree_transaction_complete_cancel (transaction, invocation);

  return TRUE;
}

static gboolean
transaction_handle_start (RPMOSTreeTransaction *transaction,
                          GDBusMethodInvocation *invocation)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (transaction);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  if (priv->started)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             RPM_OSTREED_ERROR,
                                             RPM_OSTREED_ERROR_FAILED,
                                             "Transaction has already started");
    }
  else
    {
      GTask *task;

      g_debug ("%s (%p): Started", G_OBJECT_TYPE_NAME (self), self);

      priv->started = TRUE;

      task = g_task_new (transaction,
                         priv->cancellable,
                         transaction_execute_done_cb,
                         NULL);
      g_task_run_in_thread (task, transaction_execute_thread);
      g_object_unref (task);

      rpmostree_transaction_complete_start (transaction, invocation);
    }

  return TRUE;
}

static void
rpmostreed_transaction_class_init (RpmostreedTransactionClass *class)
{
  GObjectClass *object_class;

  g_type_class_add_private (class, sizeof (RpmostreedTransactionPrivate));

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

  signals[CANCELLED] = g_signal_new ("cancelled",
                                     RPMOSTREED_TYPE_TRANSACTION,
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);

  signals[CLOSED] = g_signal_new ("closed",
                                  RPMOSTREED_TYPE_TRANSACTION,
                                  G_SIGNAL_RUN_LAST,
                                  0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);

  signals[OWNER_VANISHED] = g_signal_new ("owner-vanished",
                                          RPMOSTREED_TYPE_TRANSACTION,
                                          G_SIGNAL_RUN_LAST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
}

static void
rpmostreed_transaction_initable_iface_init (GInitableIface *iface)
{
  iface->init = transaction_initable_init;
}

static void
rpmostreed_transaction_dbus_iface_init (RPMOSTreeTransactionIface *iface)
{
  iface->handle_cancel = transaction_handle_cancel;
  iface->handle_start  = transaction_handle_start;
}

static void
rpmostreed_transaction_init (RpmostreedTransaction *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                            RPMOSTREED_TYPE_TRANSACTION,
                                            RpmostreedTransactionPrivate);
}

OstreeSysroot *
rpmostreed_transaction_get_sysroot (RpmostreedTransaction *transaction)
{
  RpmostreedTransactionPrivate *priv;

  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), NULL);

  priv = rpmostreed_transaction_get_private (transaction);

  return priv->sysroot;
}

GDBusMethodInvocation *
rpmostreed_transaction_get_invocation (RpmostreedTransaction *transaction)
{
  RpmostreedTransactionPrivate *priv;

  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), NULL);

  priv = rpmostreed_transaction_get_private (transaction);

  return priv->invocation;
}

const char *
rpmostreed_transaction_get_client_address (RpmostreedTransaction *transaction)
{
  RpmostreedTransactionPrivate *priv;

  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), NULL);

  priv = rpmostreed_transaction_get_private (transaction);

  return g_dbus_server_get_client_address (priv->server);
}

void
rpmostreed_transaction_emit_message_printf (RpmostreedTransaction *transaction,
                                            const char *format,
                                            ...)
{
  g_autofree char *formatted_message = NULL;
  va_list args;

  g_return_if_fail (RPMOSTREED_IS_TRANSACTION (transaction));
  g_return_if_fail (format != NULL);

  va_start (args, format);
  formatted_message = g_strdup_vprintf (format, args);
  va_end (args);

  rpmostree_transaction_emit_message (RPMOSTREE_TRANSACTION (transaction),
                                      formatted_message);
}

void
rpmostreed_transaction_connect_download_progress (RpmostreedTransaction *transaction,
                                                  OstreeAsyncProgress *progress)
{
  g_return_if_fail (RPMOSTREED_IS_TRANSACTION (transaction));
  g_return_if_fail (OSTREE_IS_ASYNC_PROGRESS (progress));

  g_signal_connect_object (progress,
                           "changed",
                           G_CALLBACK (transaction_progress_changed_cb),
                           transaction, 0);
}

void
rpmostreed_transaction_connect_signature_progress (RpmostreedTransaction *transaction,
                                                   OstreeRepo *repo)
{
  g_return_if_fail (RPMOSTREED_IS_TRANSACTION (transaction));
  g_return_if_fail (OSTREE_REPO (repo));

  g_signal_connect_object (repo, "gpg-verify-result",
                           G_CALLBACK (transaction_gpg_verify_result_cb),
                           transaction, 0);
}
