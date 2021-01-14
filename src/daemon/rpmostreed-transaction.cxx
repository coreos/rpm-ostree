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
#include <systemd/sd-journal.h>
#include <systemd/sd-login.h>
#include <stdexcept>

#include "rpmostreed-transaction.h"
#include "rpmostreed-errors.h"
#include "rpmostreed-sysroot.h"
#include "rpmostreed-daemon.h"

struct _RpmostreedTransactionPrivate {
  GDBusMethodInvocation *invocation;
  gboolean executed; /* TRUE if the transaction has completed (successfully or not) */
  GCancellable *cancellable;

  /* For the duration of the transaction, we hold a ref to a new
   * OstreeSysroot instance (to avoid any threading issues), and we
   * also lock it.
   */
  char *sysroot_path;
  OstreeSysroot *sysroot;
  gboolean sysroot_locked;
  /* Capture of the client description, agent, and systemd unit at txn creation time */
  char *client_description;
  char *agent_id;
  char *sd_unit;

  gboolean redirect_output;

  GDBusServer *server;
  GHashTable *peer_connections;

  /* For emitting Finished signals to late connections. */
  GVariant *finished_params;

  guint watch_id;
};

enum {
  PROP_0,
  PROP_EXECUTED,
  PROP_INVOCATION,
  PROP_SYSROOT_PATH,
  PROP_REDIRECT_OUTPUT
};

enum {
  CLOSED,
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
unlock_sysroot (RpmostreedTransaction *self)
{
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  if (!(priv->sysroot && priv->sysroot_locked))
    return;
  
  ostree_sysroot_unlock (priv->sysroot);
  sd_journal_print (LOG_INFO, "Unlocked sysroot");
  priv->sysroot_locked = FALSE;
}

static void
transaction_maybe_emit_closed (RpmostreedTransaction *self)
{
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  if (rpmostreed_transaction_get_active (self))
    return;

  if (g_hash_table_size (priv->peer_connections) > 0)
    return;

  g_signal_emit (self, signals[CLOSED], 0);

  rpmostreed_sysroot_finish_txn (rpmostreed_sysroot_get (), self);
}

static char *
creds_to_string (GCredentials *creds)
{
  auto uid = g_credentials_get_unix_user (creds, NULL);
  auto pid = g_credentials_get_unix_pid (creds, NULL);
  g_autofree char *unit = NULL;
  if (pid != -1)
    sd_pid_get_unit (pid, &unit);

  return g_strdup_printf ("[pid: %u uid: %u unit: %s]", (guint32) pid, (guint32) uid, unit ?: "(unknown)");
}

static void
transaction_connection_closed_cb (GDBusConnection *connection,
                                  gboolean remote_peer_vanished,
                                  GError *error,
                                  RpmostreedTransaction *self)
{
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  g_autofree char *creds = creds_to_string (g_dbus_connection_get_peer_credentials (connection));
  if (remote_peer_vanished)
    sd_journal_print (LOG_INFO, "Process %s disconnected from transaction progress", creds);
  else
    sd_journal_print (LOG_INFO, "Disconnecting process %s from transaction progress", creds);

  g_hash_table_remove (priv->peer_connections, connection);

  transaction_maybe_emit_closed (self);
}

static gboolean
transaction_new_connection_cb (GDBusServer *server,
                               GDBusConnection *connection,
                               RpmostreedTransaction *self)
{
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);
  GError *local_error = NULL;

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
                           self, static_cast<GConnectFlags>(0));

  g_hash_table_add (priv->peer_connections, g_object_ref (connection));

  g_autofree char *creds = creds_to_string (g_dbus_connection_get_peer_credentials (connection));
  sd_journal_print (LOG_INFO, "Process %s connected to transaction progress", creds);

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
      g_signal_emit (self, signals[CLOSED], 0);
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

  g_autofree gchar *status = NULL;

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
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);
  RpmostreedTransactionClass *clazz = RPMOSTREED_TRANSACTION_GET_CLASS (self);
  gboolean success = TRUE;
  GError *local_error = NULL;
  g_autoptr(GMainContext) mctx = g_main_context_new ();

  /* libostree iterates and calls quit on main loop
   * so we need to run in our own context.  Having a different
   * main context for worker threads should be standard practice
   * anyways.
   */
  g_main_context_push_thread_default (mctx);

  if (clazz->execute != NULL)
    {
      try {
        success = clazz->execute (self, cancellable, &local_error);
      } catch (std::exception& e) {
        success = glnx_throw (&local_error, "%s", e.what());
      }
    }

  if (local_error != NULL)
    {
      /* Also log to journal in addition to the client, so it's recorded
       * consistently.
       */
      sd_journal_print (LOG_ERR, "Txn %s on %s failed: %s",
                        g_dbus_method_invocation_get_method_name (priv->invocation),
                        g_dbus_method_invocation_get_object_path (priv->invocation),
                        local_error->message);
      g_task_return_error (task, local_error);
    }
  else
    {
      sd_journal_print (LOG_INFO, "Txn %s on %s successful",
                        g_dbus_method_invocation_get_method_name (priv->invocation),
                        g_dbus_method_invocation_get_object_path (priv->invocation));
      g_task_return_boolean (task, success);
    }

  /* Clean up context */
  g_main_context_pop_thread_default (mctx);
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
  GError **error = &local_error;
  GLNX_AUTO_PREFIX_ERROR ("During txn completion", error);

  success = g_task_propagate_boolean (G_TASK (result), &local_error);
  if (success)
    {
      if (!rpmostreed_sysroot_reload (rpmostreed_sysroot_get (), &local_error))
        success = FALSE;
    }

  /* Sanity check */
  g_warn_if_fail ((success && local_error == NULL) ||
                  (!success && local_error != NULL));

  if (local_error != NULL)
    error_message = local_error->message;

  if (error_message == NULL)
    error_message = "";

  g_debug ("%s (%p): Finished%s%s%s",
           G_OBJECT_TYPE_NAME (self), self,
           success ? "" : " (error: ",
           success ? "" : error_message,
           success ? "" : ")");

  rpmostree_transaction_emit_finished (RPMOSTREE_TRANSACTION (self),
                                       success, error_message);

  /* Stash the Finished signal parameters in case we need
   * to emit the signal again on subsequent new connections. */
  priv->finished_params = g_variant_new ("(bs)", success, error_message);
  g_variant_ref_sink (priv->finished_params);

  priv->executed = TRUE;
  unlock_sysroot (self);
  g_object_notify (G_OBJECT (self), "executed");

  transaction_maybe_emit_closed (self);
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
        priv->invocation = static_cast<GDBusMethodInvocation*>(g_value_dup_object (value));
        break;
      case PROP_SYSROOT_PATH:
        priv->sysroot_path = g_value_dup_string (value);
        break;
      case PROP_REDIRECT_OUTPUT:
        priv->redirect_output = g_value_get_boolean (value);
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
      case PROP_SYSROOT_PATH:
        g_value_set_string (value, priv->sysroot_path);
        break;
      case PROP_REDIRECT_OUTPUT:
        g_value_set_boolean (value, priv->redirect_output);
        break;
      case PROP_EXECUTED:
        g_value_set_boolean (value, priv->executed);
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

  g_hash_table_remove_all (priv->peer_connections);

  g_clear_object (&priv->invocation);
  g_clear_object (&priv->cancellable);
  g_clear_object (&priv->sysroot);
  g_clear_object (&priv->server);
  g_clear_pointer (&priv->sysroot_path, g_free);

  g_clear_pointer (&priv->finished_params, (GDestroyNotify) g_variant_unref);

  G_OBJECT_CLASS (rpmostreed_transaction_parent_class)->dispose (object);
}

static void
transaction_finalize (GObject *object)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (object);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  g_debug ("%s (%p): Finalized", G_OBJECT_TYPE_NAME (self), self);
  
  unlock_sysroot (self);

  if (priv->watch_id > 0)
    g_bus_unwatch_name (priv->watch_id);

  g_hash_table_destroy (priv->peer_connections);

  g_free (priv->client_description);
  g_free (priv->agent_id);
  g_free (priv->sd_unit);

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

      /* Watch the sender's bus name until the transaction is started.
       * This guards against a process initiating a transaction but then
       * terminating before calling Start().  If the bus name vanishes
       * during this time, we abort the transaction. */
      priv->watch_id = g_bus_watch_name_on_connection (connection,
                                                       sender,
                                                       G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       NULL,
                                                       transaction_owner_vanished_cb,
                                                       self,
                                                       NULL);

      priv->client_description = rpmostreed_daemon_client_get_string (rpmostreed_daemon_get(), sender);
      priv->agent_id = rpmostreed_daemon_client_get_agent_id (rpmostreed_daemon_get(), sender);
      priv->sd_unit = rpmostreed_daemon_client_get_sd_unit (rpmostreed_daemon_get(), sender);
      rpmostree_transaction_set_initiating_client_description ((RPMOSTreeTransaction*)self, priv->client_description);
    }
}

static gboolean
foreach_close_peer (gpointer key,
                    gpointer value,
                    gpointer user_data)
{
  auto conn = static_cast<GDBusConnection *>(key);
  g_dbus_connection_close_sync (conn, NULL, NULL);
  return TRUE;
}

void
rpmostreed_transaction_force_close (RpmostreedTransaction *transaction)
{
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (transaction);
  g_assert (priv->executed);
  g_dbus_server_stop (priv->server);
  g_hash_table_foreach_remove (priv->peer_connections, foreach_close_peer, NULL);
}

static void
on_sysroot_journal_msg (OstreeSysroot *sysroot,
                        const char    *msg,
                        void          *opaque)
{
  rpmostree_transaction_emit_message (RPMOSTREE_TRANSACTION (opaque), msg);
}


static gboolean
transaction_initable_init (GInitable *initable,
                           GCancellable *cancellable,
                           GError **error)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (initable);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  if (G_IS_CANCELLABLE (cancellable))
    priv->cancellable = (GCancellable*)g_object_ref (cancellable);

  /* Set up a private D-Bus server over which to emit
   * progress and informational messages to the caller. */

  g_autofree char *guid = g_dbus_generate_guid ();
  priv->server = g_dbus_server_new_sync ("unix:tmpdir=/tmp/rpm-ostree",
                                         G_DBUS_SERVER_FLAGS_NONE,
                                         guid,
                                         NULL,
                                         cancellable,
                                         error);
  if (priv->server == NULL)
    return FALSE;

  g_signal_connect_object (priv->server,
                           "new-connection",
                           G_CALLBACK (transaction_new_connection_cb),
                           self, static_cast<GConnectFlags>(0));

  if (priv->sysroot_path != NULL)
    {
      g_autoptr(GFile) tmp_path = g_file_new_for_path (priv->sysroot_path);
      gboolean lock_acquired = FALSE;

      /* We create a *new* sysroot to avoid threading issues like data
       * races - OstreeSysroot has no internal locking.  Efficiency
       * could be improved with a "clone" operation to avoid reloading
       * everything from disk.
       */
      priv->sysroot = ostree_sysroot_new (tmp_path);
      /* See also related code in rpmostreed-sysroot.c */
      if (!ostree_sysroot_initialize (priv->sysroot, error))
        return FALSE;
      /* We use MountFlags=slave in the unit file, which combined
      * with this ensures we support read-only /sysroot mounts.
      * https://github.com/ostreedev/ostree/issues/1265
      **/
      ostree_sysroot_set_mount_namespace_in_use (priv->sysroot);
      g_signal_connect (priv->sysroot, "journal-msg",
                        G_CALLBACK (on_sysroot_journal_msg), self);

      if (!ostree_sysroot_load (priv->sysroot, cancellable, error))
        return FALSE;

      if (!ostree_sysroot_try_lock (priv->sysroot, &lock_acquired, error))
        return FALSE;

      if (!lock_acquired)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                               "System transaction in progress");
          return FALSE;
        }

      priv->sysroot_locked = TRUE;
      sd_journal_print (LOG_INFO, "Locked sysroot");
    }

  g_dbus_server_start (priv->server);

  g_debug ("%s (%p): Initialized, listening on %s",
           G_OBJECT_TYPE_NAME (self), self,
           rpmostreed_transaction_get_client_address (self));

  return TRUE;
}

static gboolean
transaction_handle_cancel (RPMOSTreeTransaction *transaction,
                           GDBusMethodInvocation *invocation)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (transaction);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);

  g_debug ("%s (%p): Cancelled", G_OBJECT_TYPE_NAME (self), self);

  g_cancellable_cancel (priv->cancellable);

  rpmostree_transaction_complete_cancel (transaction, invocation);

  return TRUE;
}

static gboolean
transaction_handle_start (RPMOSTreeTransaction *transaction,
                          GDBusMethodInvocation *invocation)
{
  RpmostreedTransaction *self = RPMOSTREED_TRANSACTION (transaction);
  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (self);
  gboolean started = FALSE;

  /* The bus name watch ID doubles as a "not-yet-started" flag.
   * Once started the transaction proceeds independently of the
   * initiating process whose bus name we were watching. */
  if (priv->watch_id > 0)
    {
      started = TRUE;

      g_debug ("%s (%p): Started", G_OBJECT_TYPE_NAME (self), self);

      g_bus_unwatch_name (priv->watch_id);
      priv->watch_id = 0;

      GTask *task = g_task_new (transaction,
                         priv->cancellable,
                         transaction_execute_done_cb,
                         NULL);
      /* Some of the async ops in rpmostree-core.c will cancel,
       * but we want the first error to take precedence.
       */
      g_task_set_check_cancellable (task, FALSE);
      g_task_run_in_thread (task, transaction_execute_thread);
      g_object_unref (task);
    }

  rpmostree_transaction_complete_start (transaction, invocation, started);

  /* If the transaction is already finished, emit the
   * Finished signal again but only on this connection. */
  if (priv->finished_params != NULL)
    {
      GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
      const char *object_path = g_dbus_method_invocation_get_object_path (invocation);
      const char *interface_name = g_dbus_method_invocation_get_interface_name (invocation);

      GError *local_error = NULL;
      g_dbus_connection_emit_signal (connection,
                                     NULL,
                                     object_path,
                                     interface_name,
                                     "Finished",
                                     priv->finished_params,
                                     &local_error);

      if (local_error != NULL)
        {
          g_critical ("%s", local_error->message);
          g_clear_error (&local_error);
        }
    }

  return TRUE;
}

static void
rpmostreed_transaction_class_init (RpmostreedTransactionClass *clazz)
{
  GObjectClass *object_class;

  g_type_class_add_private (clazz, sizeof (RpmostreedTransactionPrivate));

  object_class = G_OBJECT_CLASS (clazz);
  object_class->set_property = transaction_set_property;
  object_class->get_property = transaction_get_property;
  object_class->dispose = transaction_dispose;
  object_class->finalize = transaction_finalize;
  object_class->constructed = transaction_constructed;

  g_object_class_install_property (object_class,
                                   PROP_EXECUTED,
                                   g_param_spec_boolean ("executed",
                                                         "Executed",
                                                         "Whether the transaction has finished",
                                                         FALSE,
                                                         static_cast<GParamFlags>(G_PARAM_READABLE |
                                                         G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (object_class,
                                   PROP_INVOCATION,
                                   g_param_spec_object ("invocation",
                                                        "Invocation",
                                                        "D-Bus method invocation",
                                                        G_TYPE_DBUS_METHOD_INVOCATION,
                                                        static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (object_class,
                                   PROP_SYSROOT_PATH,
                                   g_param_spec_string ("sysroot-path",
                                                        "Sysroot path",
                                                        "An OstreeSysroot path",
                                                        "",
                                                        static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (object_class,
                                   PROP_REDIRECT_OUTPUT,
                                   g_param_spec_boolean ("output-to-self",
                                                         "Output to self",
                                                         "Whether to redirect output to daemon itself",
                                                         FALSE,
                                                         static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS)));

  signals[CLOSED] = g_signal_new ("closed",
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
  g_assert (!rpmostreed_sysroot_has_txn (rpmostreed_sysroot_get ()));
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                            RPMOSTREED_TYPE_TRANSACTION,
                                            RpmostreedTransactionPrivate);

  self->priv->peer_connections = g_hash_table_new_full (g_direct_hash,
                                                        g_direct_equal,
                                                        g_object_unref,
                                                        NULL);
}

gboolean
rpmostreed_transaction_get_active (RpmostreedTransaction *transaction)
{
  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), FALSE);

  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (transaction);
  return (priv->finished_params == NULL);
}

OstreeSysroot *
rpmostreed_transaction_get_sysroot (RpmostreedTransaction *transaction)
{
  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), NULL);

  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (transaction);
  return priv->sysroot;
}

const char *
rpmostreed_transaction_get_client (RpmostreedTransaction *transaction)
{
  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), NULL);

  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (transaction);
  return priv->client_description;
}

const char *
rpmostreed_transaction_get_agent_id (RpmostreedTransaction *transaction)
{
  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), NULL);

  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (transaction);
  return priv->agent_id;
}

const char *
rpmostreed_transaction_get_sd_unit (RpmostreedTransaction *transaction)
{
  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), NULL);

  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (transaction);
  return priv->sd_unit;
}

GDBusMethodInvocation *
rpmostreed_transaction_get_invocation (RpmostreedTransaction *transaction)
{
  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), NULL);

  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (transaction);
  return priv->invocation;
}

const char *
rpmostreed_transaction_get_client_address (RpmostreedTransaction *transaction)
{

  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), NULL);

  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (transaction);
  return g_dbus_server_get_client_address (priv->server);
}

gboolean
rpmostreed_transaction_is_compatible (RpmostreedTransaction *transaction,
                                      GDBusMethodInvocation *invocation)
{
  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION (transaction), FALSE);
  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), FALSE);

  RpmostreedTransactionPrivate *priv = rpmostreed_transaction_get_private (transaction);

  const char *method_name_a = g_dbus_method_invocation_get_method_name (priv->invocation);
  const char *method_name_b = g_dbus_method_invocation_get_method_name (invocation);

  GVariant *parameters_a = g_dbus_method_invocation_get_parameters (priv->invocation);
  GVariant *parameters_b = g_dbus_method_invocation_get_parameters (invocation);
  return g_str_equal (method_name_a, method_name_b) &&
         g_variant_equal (parameters_a, parameters_b);
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
                           transaction, static_cast<GConnectFlags>(0));
}

void
rpmostreed_transaction_connect_signature_progress (RpmostreedTransaction *transaction,
                                                   OstreeRepo *repo)
{
  g_return_if_fail (RPMOSTREED_IS_TRANSACTION (transaction));
  g_return_if_fail (OSTREE_REPO (repo));

  g_signal_connect_object (repo, "gpg-verify-result",
                           G_CALLBACK (transaction_gpg_verify_result_cb),
                           transaction, static_cast<GConnectFlags>(0));
}
