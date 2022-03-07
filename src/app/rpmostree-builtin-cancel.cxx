/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <glib-unix.h>
#include <string.h>

#include "rpmostree-builtins.h"
#include "rpmostree-clientlib.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-util.h"

#include <libglnx.h>

static GOptionEntry option_entries[] = { { NULL } };

static void
on_cancel_method_completed (GObject *src, GAsyncResult *res, gpointer user_data)
{
  /* Nothing right now - we ideally should check for an error, but the
   * transaction could have gone away. A better fix here would be to keep
   * transactions around for some period of time.
   */
  (void)rpmostree_transaction_call_cancel_finish ((RPMOSTreeTransaction *)src, res, NULL);
}

static void
on_active_txn_path_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  auto donep = static_cast<gboolean *> (user_data);
  *donep = TRUE;
  g_main_context_wakeup (NULL);
}

gboolean
rpmostree_builtin_cancel (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                          GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("");
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, invocation,
                                       cancellable, NULL, NULL, &sysroot_proxy, error))
    return FALSE;

  /* Keep track of the txn path we saw first, as well as checking if it changed
   * over time.
   */
  g_autofree char *txn_path = NULL;
  glnx_unref_object RPMOSTreeTransaction *txn_proxy = NULL;
  if (!rpmostree_transaction_connect_active (sysroot_proxy, &txn_path, &txn_proxy, cancellable,
                                             error))
    return FALSE;
  if (!txn_proxy)
    {
      /* Let's not make this an error; cancellation may race with completion.
       * Perhaps in the future we could try to see whether a txn exited
       * "recently" but eh.
       */
      g_print ("No active transaction.\n");
      return TRUE;
    }

  const char *title = rpmostree_transaction_get_title (txn_proxy);
  g_print ("Cancelling transaction: %s\n", title);

  /* Asynchronously cancel, waiting for the sysroot property to change */
  rpmostree_transaction_call_cancel (txn_proxy, cancellable, on_cancel_method_completed, NULL);

  g_clear_object (&txn_proxy);

  gboolean done = FALSE;
  g_signal_connect (sysroot_proxy, "notify::active-transaction-path",
                    G_CALLBACK (on_active_txn_path_changed), &done);
  /* Wait for the transaction to go away */
  while (!done)
    {
      const char *current_txn_path = rpmostree_sysroot_get_active_transaction_path (sysroot_proxy);
      if (g_strcmp0 (txn_path, current_txn_path) != 0)
        break;
      g_main_context_iteration (NULL, TRUE);
    }
  g_print ("Cancelled.\n");

  return TRUE;
}
