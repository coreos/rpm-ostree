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
#include "types.h"
#include "auth.h"
#include "errors.h"
#include "daemon.h"

#include <libglnx.h>

/**
 * auth_check_root_or_access_denied:
 *
 * Used with the "g-authorize-method" signal.
 * returns a gboolean represening if the user
 * is root.
 */
gboolean
auth_check_root_or_access_denied (GDBusInterfaceSkeleton *instance,
                                  GDBusMethodInvocation *invocation,
                                  gpointer user_data)
{
  const gchar *sender;
  gboolean ret = FALSE;

  g_autoptr (GVariant) value = NULL;
  GError *error = NULL;
  GDBusConnection *connection = NULL;
  guint32 uid = UINT32_MAX;

  if (!daemon_on_message_bus (daemon_get ()))
    {
      ret = TRUE;
      goto out;
    }

  sender = g_dbus_method_invocation_get_sender (invocation);
  connection = g_dbus_method_invocation_get_connection (invocation);

  g_return_val_if_fail (sender != NULL, FALSE);

  g_debug ("Checking auth");

  value = g_dbus_connection_call_sync (connection,
                                       "org.freedesktop.DBus",
                                       "/org/freedesktop/DBus",
                                       "org.freedesktop.DBus",
                                       "GetConnectionUnixUser",
                                       g_variant_new ("(s)", sender),
                                       G_VARIANT_TYPE ("(u)"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       NULL,
                                       &error);

  if (error != NULL)
    {
      g_critical ("Couldn't get uid for '%s': %s",
                  sender, error->message);
      goto out;
    }

  g_variant_get (value, "(u)", &uid);
  ret = uid == 0;

out:
  if (!ret)
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     RPM_OSTREED_ERROR,
                                                     RPM_OSTREED_ERROR_NOT_AUTHORIZED,
                                                     "Access Denied");
    }

  g_clear_error (&error);
  return ret;
}
