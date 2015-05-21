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

#include "utils.h"

static gboolean
handle_cancel_cb (RPMOSTreeTransaction *transaction,
                  GDBusMethodInvocation *invocation,
                  GCancellable *method_cancellable)
{
  g_cancellable_cancel (method_cancellable);

  rpmostree_transaction_complete_cancel (transaction, invocation);

  return TRUE;
}

RPMOSTreeTransaction *
new_transaction (GDBusMethodInvocation *invocation,
                 GCancellable *method_cancellable,
                 GError **error)
{
  RPMOSTreeTransaction *transaction;
  GDBusConnection *connection;
  const char *method_name;
  const char *object_path;
  const char *sender;
  g_autofree gchar *child_object_path = NULL;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), FALSE);

  connection = g_dbus_method_invocation_get_connection (invocation);
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

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (transaction),
                                         connection,
                                         child_object_path,
                                         error))
    {
      g_clear_object (&transaction);
    }

  return transaction;
}
