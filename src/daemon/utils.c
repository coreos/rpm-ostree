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

#include "libgsystem.h"

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


static void
append_to_object_path (GString *str,
                       const gchar *s)
{
  guint n;

  for (n = 0; s[n] != '\0'; n++)
    {
      gint c = s[n];
      /* D-Bus spec sez:
       *
       * Each element must only contain the ASCII characters "[A-Z][a-z][0-9]_-"
       */
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
        {
          g_string_append_c (str, c);
        }
      else if (c == '-' || c == '/')
        {
          /* Swap / or - for _ to keep names easier to read */
          g_string_append_c (str, '_');
        }
      else
        {
          /* Escape bytes not in [A-Z][a-z][0-9] as _<hex-with-two-digits> */
          g_string_append_printf (str, "_%02x", c & 0xFF);
        }
    }
}

/**
 * utils_generate_object_path:
 * @base: The base object path (without trailing '/').
 * @part...: UTF-8 strings.
 *
 * Appends @s to @base in a way such that only characters that can be
 * used in a D-Bus object path will be used. E.g. a character not in
 * <literal>[A-Z][a-z][0-9]_</literal> will be escaped as _HEX where
 * HEX is a two-digit hexadecimal number.
 *
 * Note that his mapping is not bijective - e.g. you cannot go back
 * to the original string.
 *
 * Returns: An allocated string that must be freed with g_free ().
 */
gchar *
utils_generate_object_path (const gchar *base,
                            const gchar *part,
                            ...)
{
  gchar *result;
  va_list va;

  va_start (va, part);
  result = utils_generate_object_path_from_va (base, part, va);
  va_end (va);

  return result;
}

gchar *
utils_generate_object_path_from_va (const gchar *base,
                              const gchar *part,
                              va_list va)
{
  GString *path;

  g_return_val_if_fail (base != NULL, NULL);
  g_return_val_if_fail (g_variant_is_object_path (base), NULL);
  g_return_val_if_fail (!g_str_has_suffix (base, "/"), NULL);

  path = g_string_new (base);

  while (part != NULL)
    {
      if (!g_utf8_validate (part, -1, NULL))
        {
          g_string_free (path, TRUE);
          return NULL;
        }
      else
        {
          g_string_append_c (path, '/');
          append_to_object_path (path, part);
          part = va_arg (va, const gchar *);
        }
    }

  return g_string_free (path, FALSE);
}

/**
 * utils_load_sysroot_and_repo:
 * @path: The path to the sysroot.
 * @cancellable: Cancelable
 * @out_sysroot (out): The OstreeSysroot at the given path
 * @out_repo (out): The OstreeRepo for the sysroot
 * @error (out): Error

 * Returns: True on success.
 */
gboolean
utils_load_sysroot_and_repo (gchar *path,
                             GCancellable *cancellable,
                             OstreeSysroot **out_sysroot,
                             OstreeRepo **out_repo,
                             GError **error)
{
  gs_unref_object GFile *sysroot_path = NULL;
  gs_unref_object OstreeSysroot *ot_sysroot = NULL;
  gboolean ret = FALSE;

  sysroot_path = g_file_new_for_path (path);
  ot_sysroot = ostree_sysroot_new (sysroot_path);

  if (!ostree_sysroot_load (ot_sysroot,
                            cancellable,
                            error))
      goto out;

  // ostree_sysroot_get_repo now just adds a
  // ref to its singleton
  if (!ostree_sysroot_get_repo (ot_sysroot,
                                out_repo,
                                cancellable,
                                error))
      goto out;

  gs_transfer_out_value (out_sysroot, &ot_sysroot);
  ret = TRUE;

out:
  return ret;
}
