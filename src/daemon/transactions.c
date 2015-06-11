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
#include "transactions.h"
#include "errors.h"
#include "daemon.h"

static guint TRANSACTION_KEEP_SECONDS = 300;

static gboolean
ensure_same_caller (RPMOSTreeTransaction *transaction,
                    GDBusMethodInvocation *invocation)
{
  gboolean ret = FALSE;
  const char *known_sender;
  const char *sender;

  sender = g_dbus_method_invocation_get_sender (invocation);
  known_sender = rpmostree_transaction_get_initiating_owner (transaction);

  if (g_strcmp0(sender, known_sender) != 0)
    {
      GError *error = g_error_new_literal (RPM_OSTREED_ERROR, RPM_OSTREED_ERROR_FAILED,
                                           "You are not allowed to cancel this transaction.");
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  ret = TRUE;

out:
  return ret;
}

static gboolean
handle_cancel_cb (RPMOSTreeTransaction *transaction,
                  GDBusMethodInvocation *invocation,
                  GCancellable *method_cancellable)
{
  if (!ensure_same_caller (transaction, invocation))
    goto out;

  g_cancellable_cancel (method_cancellable);
  rpmostree_transaction_complete_cancel (transaction, invocation);

out:
  return TRUE;
}

static void
progress_changed_cb (OstreeAsyncProgress *progress,
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
gpg_verify_result_cb (OstreeRepo *repo,
                      const char *checksum,
                      OstreeGpgVerifyResult *result,
                      RPMOSTreeTransaction *transaction)
{
  guint n, i;
  GVariantBuilder builder;

  if (rpmostree_transaction_get_complete (transaction))
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

void
transaction_connect_download_progress (RPMOSTreeTransaction *transaction,
                                       OstreeAsyncProgress *progress)
{
  g_signal_connect_object (progress,
                           "changed",
                           G_CALLBACK (progress_changed_cb),
                           transaction, 0);
}

void
transaction_connect_signature_progress (RPMOSTreeTransaction *transaction,
                                       OstreeRepo *repo)
{
  g_signal_connect_object (repo, "gpg-verify-result",
                           G_CALLBACK (gpg_verify_result_cb),
                           transaction, 0);
}

static gboolean
close_transaction (gpointer user_data)
{
  RPMOSTreeTransaction *transaction = RPMOSTREE_TRANSACTION (user_data);
  const char *object_path;

  object_path = g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON(transaction));

  if (object_path)
    {
      daemon_unpublish (daemon_get (),
                        object_path,
                        transaction);
    }
  g_clear_object (&transaction);

  return FALSE;
}

RPMOSTreeTransaction *
new_transaction (GDBusMethodInvocation *invocation,
                 GCancellable *method_cancellable,
                 GError **error)
{
  RPMOSTreeTransaction *transaction;
  const char *method_name;
  const char *object_path;
  const char *sender;
  g_autofree gchar *child_object_path = NULL;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), FALSE);

  method_name = g_dbus_method_invocation_get_method_name (invocation);
  object_path = g_dbus_method_invocation_get_object_path (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  child_object_path = g_build_path ("/", object_path, "Transaction", NULL);

  transaction = rpmostree_transaction_skeleton_new ();
  rpmostree_transaction_set_method (transaction, method_name);
  rpmostree_transaction_set_initiating_owner (transaction, sender);

  if (G_IS_CANCELLABLE (method_cancellable))
    {
      g_signal_connect_object (transaction,
                               "handle-cancel",
                               G_CALLBACK (handle_cancel_cb),
                               method_cancellable, 0);
    }

  /* Published uniquely */
  daemon_publish (daemon_get (), child_object_path, TRUE, transaction);

  return transaction;
}

void
complete_transaction (RPMOSTreeTransaction *transaction,
                      gboolean success,
                      const gchar *message)
{
  if (message != NULL)
    rpmostree_transaction_set_result_message (transaction, message);

  rpmostree_transaction_set_success (transaction, success);
  rpmostree_transaction_set_complete (transaction, TRUE);

  g_timeout_add_seconds (TRANSACTION_KEEP_SECONDS,
                         close_transaction,
                         g_object_ref (transaction));
}
