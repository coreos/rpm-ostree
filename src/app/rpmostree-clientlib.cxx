/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <err.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <systemd/sd-login.h>
#include <unistd.h>
#include <utility>

#include <glib-unix.h>
#include <libglnx.h>

#include "rpmostree-builtins.h"
#include "rpmostree-clientlib.h"
#include "rpmostree-cxxrs.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-types.h"
#include "rpmostree-util.h"
#include "rpmostreed-transaction-types.h"

static gboolean impl_transaction_get_response_sync (GDBusConnection *connection,
                                                    const char *transaction_address,
                                                    GCancellable *cancellable, GError **error);

#define RPMOSTREE_CLI_ID "cli"
// Default DBus timeout is 25s, which is quite short.  Bump it significantly to reduce
// the flake rate on heavily loaded machines.  It's tempting to bump it to infinity,
// but for now let's greatly increase it.
#define DEFAULT_DBUS_TIMEOUT_MILLIS (5 * 60 * 1000)

/* Used to close race conditions by ensuring the daemon status is up-to-date */
static void
on_reload_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  auto donep = static_cast<gboolean *> (user_data);
  *donep = TRUE;
  (void)rpmostree_sysroot_call_reload_finish ((RPMOSTreeSysroot *)src, res, NULL);
}

/* This is an async call so that gdbus handles signals for changed
 * properties. */
static void
await_reload_sync (RPMOSTreeSysroot *sysroot_proxy)
{
  gboolean done = FALSE;
  rpmostree_sysroot_call_reload (sysroot_proxy, NULL, on_reload_done, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);
}

/* Connect via DBus and register as a client to rpm-ostreed,
 * with a retry loop in case the daemon
 * is in the process of auto-exiting.
 */
static gboolean
app_load_sysroot_impl (const char *sysroot, GCancellable *cancellable, GDBusConnection **out_conn,
                       GError **error)
{
  const char *bus_name = NULL;

  ROSCXX_TRY (client_start_daemon (), error);

  g_autoptr (GDBusConnection) connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, error);
  if (!connection)
    return glnx_prefix_error (error, "Connecting to system bus");

  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  /* Try to register if we can; it doesn't matter much now since the daemon doesn't
   * auto-exit, though that might change in the future. But only register if we're active or
   * root; the daemon won't allow it otherwise. */
  uid_t uid = getuid ();
  gboolean should_register;
  if (uid == 0)
    should_register = TRUE;
  else
    {
      g_autofree char *state = NULL;
      if (sd_uid_get_state (uid, &state) >= 0)
        should_register = (g_strcmp0 (state, "active") == 0);
      else
        should_register = FALSE;
    }

  /* First, call RegisterClient directly for the well-known name, to
   * cause bus activation and allow race-free idle exit.
   * https://github.com/projectatomic/rpm-ostree/pull/606
   * If we get unlucky and try to talk to the daemon in FLUSHING
   * state, then it won't reply, and we should try again.
   */
  static const char sysroot_objpath[] = "/org/projectatomic/rpmostree1/Sysroot";
  while (should_register)
    {
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GVariantBuilder) options_builder
          = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      const char *clientid = g_getenv ("RPMOSTREE_CLIENT_ID") ?: RPMOSTREE_CLI_ID;
      g_variant_builder_add (options_builder, "{sv}", "id", g_variant_new_string (clientid));
      g_autoptr (GVariant) res = g_dbus_connection_call_sync (
          connection, bus_name, sysroot_objpath, "org.projectatomic.rpmostree1.Sysroot",
          "RegisterClient", g_variant_new ("(@a{sv})", g_variant_builder_end (options_builder)),
          (GVariantType *)"()", G_DBUS_CALL_FLAGS_NONE, DEFAULT_DBUS_TIMEOUT_MILLIS, cancellable,
          &local_error);
      if (res)
        break; /* Success! */

      if (g_dbus_error_is_remote_error (local_error))
        {
          g_autofree char *remote_err = g_dbus_error_get_remote_error (local_error);
          /* If this is true, we caught the daemon after it was doing an
           * idle exit, but while it still owned the name. Retry.
           */
          if (g_str_equal (remote_err, "org.freedesktop.DBus.Error.NoReply"))
            continue;
          /* Otherwise, fall through */
        }

      /* Something else went wrong */
      g_propagate_error (error, util::move_nullify (local_error));
      return glnx_prefix_error (error, "Failed to invoke RegisterClient");
    }

  *out_conn = util::move_nullify (connection);
  return TRUE;
}

namespace rpmostreecxx
{

std::unique_ptr<ClientConnection>
new_client_connection ()
{
  g_autoptr (GError) local_error = NULL;
  GDBusConnection *conn = NULL;

  if (!app_load_sysroot_impl ("/", NULL, &conn, &local_error))
    util::throw_gerror (local_error);
  return std::make_unique<ClientConnection> (conn);
}

// Connect to a transaction DBus and monitor its progress synchronously,
// printing output to stdout.  Add a signal handler for SIGINT to cancel
// the transaction.
void
ClientConnection::transaction_connect_progress_sync (const rust::Str address) const
{
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  auto address_c = g_strndup (address.data (), address.length ());
  if (!impl_transaction_get_response_sync (conn, address_c, cancellable, &local_error))
    {
      // In this case the caller doesn't care about the remote exception; we never
      // try to match on it.
      g_dbus_error_strip_remote_error (local_error);
      util::throw_gerror (local_error);
    }
}

} // namespace

/**
 * rpmostree_load_sysroot
 * @cancellable: A GCancellable
 * @out_sysroot: (out) Return location for sysroot
 * @error: A pointer to a GError pointer.
 *
 * Returns: True on success
 **/
gboolean
rpmostree_load_sysroot (const char *sysroot, GCancellable *cancellable,
                        RPMOSTreeSysroot **out_sysroot_proxy, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Loading sysroot", error);
  g_autoptr (GDBusConnection) connection = NULL;

  if (!app_load_sysroot_impl (sysroot, cancellable, &connection, error))
    return FALSE;

  const char *bus_name = NULL;
  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy
      = rpmostree_sysroot_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_NONE, bus_name,
                                          "/org/projectatomic/rpmostree1/Sysroot", NULL, error);
  if (sysroot_proxy == NULL)
    return FALSE;
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (sysroot_proxy), DEFAULT_DBUS_TIMEOUT_MILLIS);

  /* TODO: Change RegisterClient to also do a reload and do it async instead */
  await_reload_sync (sysroot_proxy);

  *out_sysroot_proxy = util::move_nullify (sysroot_proxy);
  return TRUE;
}

gboolean
rpmostree_load_os_proxies (RPMOSTreeSysroot *sysroot_proxy, const char *opt_osname,
                           GCancellable *cancellable, RPMOSTreeOS **out_os_proxy,
                           RPMOSTreeOSExperimental **out_osexperimental_proxy, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Creating OS client proxy", error);
  g_autofree char *os_object_path = NULL;
  if (opt_osname == NULL)
    os_object_path = rpmostree_sysroot_dup_booted (sysroot_proxy);
  if (os_object_path == NULL)
    {
      /* Usually if opt_osname is null and the property isn't
         populated that means the daemon isn't listen on the bus
         make the call anyways to get the standard error.
      */
      if (!opt_osname)
        opt_osname = "";

      if (!rpmostree_sysroot_call_get_os_sync (sysroot_proxy, opt_osname, &os_object_path,
                                               cancellable, error))
        return FALSE;
    }

  /* owned by sysroot_proxy */
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));
  const char *bus_name = NULL;
  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  glnx_unref_object RPMOSTreeOS *os_proxy = rpmostree_os_proxy_new_sync (
      connection, G_DBUS_PROXY_FLAGS_NONE, bus_name, os_object_path, cancellable, error);
  if (os_proxy == NULL)
    return FALSE;
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (os_proxy), DEFAULT_DBUS_TIMEOUT_MILLIS);

  glnx_unref_object RPMOSTreeOSExperimental *ret_osexperimental_proxy = NULL;
  if (out_osexperimental_proxy)
    {
      ret_osexperimental_proxy = rpmostree_osexperimental_proxy_new_sync (
          connection, G_DBUS_PROXY_FLAGS_NONE, bus_name, os_object_path, cancellable, error);
      if (!ret_osexperimental_proxy)
        return FALSE;
      g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (ret_osexperimental_proxy),
                                        DEFAULT_DBUS_TIMEOUT_MILLIS);
    }

  *out_os_proxy = util::move_nullify (os_proxy);
  if (out_osexperimental_proxy)
    *out_osexperimental_proxy = util::move_nullify (ret_osexperimental_proxy);
  return TRUE;
}

gboolean
rpmostree_load_os_proxy (RPMOSTreeSysroot *sysroot_proxy, gchar *opt_osname,
                         GCancellable *cancellable, RPMOSTreeOS **out_os_proxy, GError **error)
{
  return rpmostree_load_os_proxies (sysroot_proxy, opt_osname, cancellable, out_os_proxy, NULL,
                                    error);
}

typedef struct
{
  gboolean progress;
  GError *error;
  GMainLoop *loop;
  gboolean complete;
} TransactionProgress;

static TransactionProgress *
transaction_progress_new (void)
{
  TransactionProgress *self;

  self = g_slice_new0 (TransactionProgress);
  self->loop = g_main_loop_new (NULL, FALSE);

  return self;
}

static void
transaction_progress_free (TransactionProgress *self)
{
  if (self == NULL)
    return;

  g_main_loop_unref (self->loop);
  g_slice_free (TransactionProgress, self);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (TransactionProgress, transaction_progress_free)

static void
transaction_progress_end (TransactionProgress *self)
{
  if (self->progress)
    {
      rpmostreecxx::console_progress_end (rust::Str ());
      self->progress = FALSE;
    }
  g_main_loop_quit (self->loop);
}

static void
on_transaction_progress (GDBusProxy *proxy, gchar *sender_name, gchar *signal_name,
                         GVariant *parameters, gpointer user_data)
{
  auto tp = static_cast<TransactionProgress *> (user_data);

  g_debug ("txn progress %s", signal_name);

  if (g_strcmp0 (signal_name, "Finished") == 0)
    {
      if (tp->error == NULL)
        {
          g_autofree char *error_message = NULL;
          gboolean success = FALSE;

          g_variant_get (parameters, "(bs)", &success, &error_message);

          if (!success)
            {
              tp->error = g_dbus_error_new_for_dbus_error (
                  "org.projectatomic.rpmostreed.Error.Failed", error_message);
            }
        }

      transaction_progress_end (tp);
      return;
    }

  // Everything else below here is just printing progress, which we suppress in quiet mode
  if (rpmostree_global_quiet ())
    return;

  if (g_strcmp0 (signal_name, "SignatureProgress") == 0)
    {
      /* We used to print the signature here, but doing so interferes with the
       * libostree HTTP progress, and it gets really, really verbose when doing
       * a deploy. Let's follow the Unix philosophy here: silence is success.
       */
    }
  else if (g_strcmp0 (signal_name, "Message") == 0)
    {
      const gchar *message = NULL;
      g_variant_get_child (parameters, 0, "&s", &message);
      g_print ("%s\n", message);
    }
  else if (g_strcmp0 (signal_name, "TaskBegin") == 0)
    {
      const gchar *message = NULL;
      g_variant_get_child (parameters, 0, "&s", &message);
      tp->progress = TRUE;
      rpmostreecxx::console_progress_begin_task (message);
    }
  else if (g_strcmp0 (signal_name, "TaskEnd") == 0)
    {
      const gchar *message = NULL;
      g_variant_get_child (parameters, 0, "&s", &message);
      if (tp->progress)
        {
          g_assert (tp->progress);
          rpmostreecxx::console_progress_end (message);
          tp->progress = FALSE;
        }
    }
  else if (g_strcmp0 (signal_name, "ProgressEnd") == 0)
    {
      if (tp->progress)
        {
          g_assert (tp->progress);
          rpmostreecxx::console_progress_end (rust::Str ());
          tp->progress = FALSE;
        }
    }
  else if (g_strcmp0 (signal_name, "PercentProgress") == 0)
    {
      const gchar *message = NULL;
      guint32 percentage;
      g_variant_get_child (parameters, 0, "&s", &message);
      g_variant_get_child (parameters, 1, "u", &percentage);
      if (!tp->progress)
        {
          tp->progress = TRUE;
          rpmostreecxx::console_progress_begin_percent (message);
        }
      rpmostreecxx::console_progress_update (percentage);
    }
  else if (g_strcmp0 (signal_name, "DownloadProgress") == 0)
    {
      auto line = rpmostreecxx::client_render_download_progress (*parameters);
      if (!tp->progress)
        {
          tp->progress = TRUE;
          rpmostreecxx::console_progress_begin_task (line.c_str ());
        }
      else
        rpmostreecxx::console_progress_set_message (line.c_str ());
    }
}

static void
on_owner_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  /* Owner shouldn't change during a transaction
   * that messes with notifications, abort, abort.
   */
  auto tp = static_cast<TransactionProgress *> (user_data);
  tp->error = g_dbus_error_new_for_dbus_error ("org.projectatomic.rpmostreed.Error.Failed",
                                               "Bus owner changed, aborting. This likely "
                                               "means the daemon crashed; check logs with "
                                               "`journalctl -xe`.");
  transaction_progress_end (tp);
}

static void
cancelled_handler (GCancellable *cancellable, gpointer user_data)
{
  auto transaction = static_cast<RPMOSTreeTransaction *> (user_data);
  rpmostree_transaction_call_cancel_sync (transaction, NULL, NULL);
}

static gboolean
on_sigint (gpointer user_data)
{
  auto cancellable = static_cast<GCancellable *> (user_data);
  if (!g_cancellable_is_cancelled (cancellable))
    {
      g_printerr ("Caught SIGINT, cancelling transaction\n");
      g_cancellable_cancel (cancellable);
    }
  else
    {
      g_printerr ("Awaiting transaction cancellation...\n");
    }
  return TRUE;
}

static gboolean
set_variable_false (gpointer data)
{
  auto donep = static_cast<int *> (data);
  g_atomic_int_set (donep, 1);
  g_main_context_wakeup (NULL);
  return FALSE;
}

/* We explicitly run the loop so we receive DBus messages,
 * in particular notification of a new txn.
 */
static void
spin_mainloop_for_a_second (void)
{
  int done = 0;

  g_timeout_add_seconds (1, set_variable_false, &done);
  while (g_atomic_int_get (&done) == 0)
    g_main_context_iteration (NULL, TRUE);
}

static RPMOSTreeTransaction *
transaction_connect (const char *transaction_address, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Failed to connect to client transaction", error);
  g_autoptr (GDBusConnection) peer_connection = g_dbus_connection_new_for_address_sync (
      transaction_address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, cancellable, error);

  if (peer_connection == NULL)
    return NULL;

  return rpmostree_transaction_proxy_new_sync (peer_connection, G_DBUS_PROXY_FLAGS_NONE, NULL, "/",
                                               cancellable, error);
}

/* Connect to the active transaction if one exists.  Because this is
 * currently racy, we use a retry loop for up to ~5 seconds.
 */
gboolean
rpmostree_transaction_connect_active (RPMOSTreeSysroot *sysroot_proxy, char **out_path,
                                      RPMOSTreeTransaction **out_txn, GCancellable *cancellable,
                                      GError **error)
{
  /* We don't want to loop infinitely if something is going wrong with e.g.
   * permissions.
   */
  guint n_tries = 0;
  const guint max_tries = 5;
  g_autoptr (GError) txn_connect_error = NULL;

  for (n_tries = 0; n_tries < max_tries; n_tries++)
    {
      const char *txn_path = rpmostree_sysroot_get_active_transaction_path (sysroot_proxy);
      if (!txn_path || !*txn_path)
        {
          /* No active txn?  We're done */
          if (out_path)
            *out_path = NULL;
          *out_txn = NULL;
          return TRUE;
        }

      /* Keep track of the last error so we have something to return */
      g_clear_error (&txn_connect_error);
      RPMOSTreeTransaction *txn = transaction_connect (txn_path, cancellable, &txn_connect_error);
      if (txn)
        {
          if (out_path)
            *out_path = g_strdup (txn_path);
          *out_txn = txn;
          return TRUE;
        }
      else
        spin_mainloop_for_a_second ();
    }

  g_propagate_error (error, util::move_nullify (txn_connect_error));
  return FALSE;
}

static void
transaction_disconnect (RPMOSTreeTransaction *transaction, guint signal_id, gulong signal_handler)
{
  g_assert (transaction != NULL);

  if (signal_id > 0)
    g_source_remove (signal_id);

  if (signal_handler > 0)
    g_signal_handler_disconnect (transaction, signal_handler);

  return;
}

/* Transactions need an explicit Start call so we can set up watches for signals
 * beforehand and avoid losing information.  We monitor the transaction,
 * printing output it sends, and handle Ctrl-C, etc.
 */
static gboolean
impl_transaction_get_response_sync (GDBusConnection *connection, const char *transaction_address,
                                    GCancellable *cancellable, GError **error)
{
  glnx_unref_object GDBusObjectManager *object_manager = NULL;
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;

  g_autoptr (TransactionProgress) tp = transaction_progress_new ();

  const char *bus_name = NULL;
  gboolean just_started = FALSE;

  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  /* If we are on the message bus, setup object manager connection
   * to notify if the owner changes. */
  if (bus_name != NULL)
    {
      object_manager = rpmostree_object_manager_client_new_sync (
          connection, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, bus_name,
          "/org/projectatomic/rpmostree1", cancellable, error);

      if (object_manager == NULL)
        return glnx_prefix_error (error, "Failed to initialize object manager client");

      g_signal_connect (object_manager, "notify::name-owner", G_CALLBACK (on_owner_changed), tp);
    }

  ROSCXX_TRY (failpoint ("client::connect"), error);

  transaction = transaction_connect (transaction_address, cancellable, error);
  if (!transaction)
    return FALSE;

  guint sigintid = g_unix_signal_add (SIGINT, on_sigint, cancellable);

  /* setup cancel handler */
  gint cancel_handler
      = g_cancellable_connect (cancellable, G_CALLBACK (cancelled_handler), transaction, NULL);

  gulong signal_handler
      = g_signal_connect (transaction, "g-signal", G_CALLBACK (on_transaction_progress), tp);

  /* Tell the server we're ready to receive signals. */
  if (!rpmostree_transaction_call_start_sync (transaction, &just_started, cancellable, error))
    {
      transaction_disconnect (transaction, sigintid, signal_handler);
      return glnx_prefix_error (error, "Failed to start transaction");
    }

  if (!just_started)
    g_print ("Attaching to existing transaction: %s\n",
             rpmostree_transaction_get_title (transaction));
  g_main_loop_run (tp->loop);

  gboolean success = FALSE;
  g_cancellable_disconnect (cancellable, cancel_handler);
  if (!g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      if (tp->error)
        {
          g_propagate_error (error, tp->error);
        }
      else
        {
          success = TRUE;
        }
    }

  transaction_disconnect (transaction, sigintid, signal_handler);

  return success;
}

/* Transactions need an explicit Start call so we can set up watches for signals
 * beforehand and avoid losing information.  We monitor the transaction,
 * printing output it sends, and handle Ctrl-C, etc.
 */
gboolean
rpmostree_transaction_get_response_sync (RPMOSTreeSysroot *sysroot_proxy,
                                         const char *transaction_address, GCancellable *cancellable,
                                         GError **error)
{
  auto connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));
  if (!impl_transaction_get_response_sync (connection, transaction_address, cancellable, error))
    return FALSE;

  /* On success, call Reload() as a way to sync with the daemon. Do this in async mode so
   * that gdbus handles signals for changed properties. */
  await_reload_sync (sysroot_proxy);

  return TRUE;
}

/* Handles client-side processing for most command line tools
 * after a transaction has been started.  Wraps invocation
 * of rpmostree_transaction_get_response_sync().
 */
gboolean
rpmostree_transaction_client_run (RpmOstreeCommandInvocation *invocation,
                                  RPMOSTreeSysroot *sysroot_proxy, RPMOSTreeOS *os_proxy,
                                  GVariant *options, gboolean exit_unchanged_77,
                                  const char *transaction_address, GVariant *previous_deployment,
                                  GCancellable *cancellable, GError **error)
{
  /* Wait for the txn to complete */
  if (!rpmostree_transaction_get_response_sync (sysroot_proxy, transaction_address, cancellable,
                                                error))
    return FALSE;

  /* Process the result of the txn and our options */

  g_auto (GVariantDict) optdict = G_VARIANT_DICT_INIT (options);
  /* Parse back the options variant */
  gboolean opt_reboot = FALSE;
  g_variant_dict_lookup (&optdict, "reboot", "b", &opt_reboot);
  gboolean opt_dry_run = FALSE;
  g_variant_dict_lookup (&optdict, "dry-run", "b", &opt_dry_run);
  gboolean opt_print_interactive = FALSE;
  g_variant_dict_lookup (&optdict, "print-interactive", "b", &opt_print_interactive);
  gboolean opt_apply_live = FALSE;
  g_variant_dict_lookup (&optdict, "apply-live", "b", &opt_apply_live);

  if (opt_print_interactive)
    {
      /* Nothing; we print the transaction by default */
    }
  else if (opt_dry_run)
    {
      g_print ("Exiting because of '--dry-run' option\n");
    }
  else if (!opt_reboot)
    {
      if (!rpmostree_has_new_default_deployment (os_proxy, previous_deployment))
        {
          if (exit_unchanged_77)
            invocation->exit_code = RPM_OSTREE_EXIT_UNCHANGED;
          return TRUE;
        }
      else if (!opt_apply_live)
        {
          /* do diff without dbus: https://github.com/projectatomic/rpm-ostree/pull/116 */
          const char *sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);
          ROSCXX_TRY (print_treepkg_diff_from_sysroot_path (
                          rust::Str (sysroot_path), RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE, 0,
                          cancellable),
                      error);
          g_print ("Changes queued for next boot. Run \"systemctl reboot\" to start a reboot\n");
        }
      else if (opt_apply_live)
        {
          const char *sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);
          g_autoptr (GFile) sysroot_file = g_file_new_for_path (sysroot_path);
          g_autoptr (OstreeSysroot) sysroot = ostree_sysroot_new (sysroot_file);
          if (!ostree_sysroot_load (sysroot, cancellable, error))
            return FALSE;
          ROSCXX_TRY (applylive_finish (*sysroot), error);
        }
      else
        g_assert_not_reached ();
    }

  return TRUE;
}

static void
rpmostree_print_signatures (GVariant *variant, const gchar *sep, gboolean verbose)
{
  const guint n_sigs = g_variant_n_children (variant);
  g_autoptr (GString) sigs_buffer = g_string_sized_new (256);

  for (guint i = 0; i < n_sigs; i++)
    {
      g_autoptr (GVariant) v = NULL;
      if (i != 0)
        g_string_append_c (sigs_buffer, '\n');
      g_variant_get_child (variant, i, "v", &v);
      if (verbose)
        ostree_gpg_verify_result_describe_variant (v, sigs_buffer, sep,
                                                   OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
      else
        {
          if (i != 0)
            g_string_append (sigs_buffer, sep);

          gboolean is_key_missing;
          g_variant_get_child (v, OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING, "b", &is_key_missing);

          const char *fingerprint;
          g_variant_get_child (v, OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT, "&s", &fingerprint);

          if (is_key_missing)
            {
              g_string_append_printf (
                  sigs_buffer, "Can't check signature: public key %s not found\n", fingerprint);
            }
          else
            {
              gboolean valid;
              g_variant_get_child (v, OSTREE_GPG_SIGNATURE_ATTR_VALID, "b", &valid);

              g_string_append_printf (sigs_buffer, "%s signature by %s\n",
                                      valid ? "Valid" : "Invalid", fingerprint);
            }
        }
    }

  g_print ("%s", sigs_buffer->str);
}

void
rpmostree_print_gpg_info (GVariant *signatures, gboolean verbose, guint max_key_len)
{
  if (signatures)
    {
      /* +2 for initial leading spaces */
      const guint gpgpad = max_key_len + 2 + strlen (": ");
      char gpgspaces[gpgpad + 1];
      memset (gpgspaces, ' ', gpgpad);
      gpgspaces[gpgpad] = '\0';

      if (verbose)
        {
          const guint n_sigs = g_variant_n_children (signatures);
          g_autofree char *gpgheader
              = g_strdup_printf ("%u signature%s", n_sigs, n_sigs == 1 ? "" : "s");
          rpmostree_print_kv ("GPGSignature", max_key_len, gpgheader);
        }
      else
        rpmostree_print_kv_no_newline ("GPGSignature", max_key_len, "");
      rpmostree_print_signatures (signatures, gpgspaces, verbose);
    }
  else
    {
      rpmostree_print_kv ("GPGSignature", max_key_len, "(unsigned)");
    }
}

static gint
pkg_diff_variant_compare (gconstpointer a, gconstpointer b, gpointer unused)
{
  const char *pkg_name_a = NULL;
  const char *pkg_name_b = NULL;

  g_variant_get_child ((GVariant *)a, 0, "&s", &pkg_name_a);
  g_variant_get_child ((GVariant *)b, 0, "&s", &pkg_name_b);

  /* XXX Names should be unique since we're comparing packages
   *     from two different trees... right? */

  return g_strcmp0 (pkg_name_a, pkg_name_b);
}

static void
pkg_diff_variant_print (GVariant *variant)
{
  g_autoptr (GVariant) details = NULL;
  const char *old_name, *old_evr, *old_arch;
  const char *new_name, *new_evr, *new_arch;
  gboolean have_old = FALSE;
  gboolean have_new = FALSE;

  details = g_variant_get_child_value (variant, 2);
  g_assert (details != NULL);

  have_old
      = g_variant_lookup (details, "PreviousPackage", "(&s&s&s)", &old_name, &old_evr, &old_arch);

  have_new = g_variant_lookup (details, "NewPackage", "(&s&s&s)", &new_name, &new_evr, &new_arch);

  if (have_old && have_new)
    {
      g_print ("!%s-%s-%s\n", old_name, old_evr, old_arch);
      g_print ("=%s-%s-%s\n", new_name, new_evr, new_arch);
    }
  else if (have_old)
    {
      g_print ("-%s-%s-%s\n", old_name, old_evr, old_arch);
    }
  else if (have_new)
    {
      g_print ("+%s-%s-%s\n", new_name, new_evr, new_arch);
    }
}

void
rpmostree_print_package_diffs (GVariant *variant)
{
  GQueue queue = G_QUEUE_INIT;
  GVariantIter iter;
  GVariant *child;

  /* GVariant format should be a(sua{sv}) */

  g_assert (variant != NULL);

  g_variant_iter_init (&iter, variant);

  /* Queue takes ownership of the child variant. */
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    g_queue_insert_sorted (&queue, child, pkg_diff_variant_compare, NULL);

  while (!g_queue_is_empty (&queue))
    {
      child = (GVariant *)g_queue_pop_head (&queue);
      pkg_diff_variant_print (child);
      g_variant_unref (child);
    }
}

/* swiss-army knife: takes an strv of pkgspecs destined for
 * install, and splits it into repo pkgs, and for local
 * pkgs, an fd list & idx variant. */
gboolean
rpmostree_sort_pkgs_strv (const char *const *pkgs, GUnixFDList *fd_list, gboolean is_replace,
                          GPtrArray **out_repo_pkgs, GVariant **out_fd_idxs, GError **error)
{
  g_autoptr (GPtrArray) repo_pkgs = g_ptr_array_new_with_free_func (g_free);
  g_auto (GVariantBuilder) builder;
  // TODO: better API/cache for this
  g_autoptr (DnfContext) ctx = dnf_context_new ();
  auto basearch = dnf_context_get_base_arch (ctx);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("ah"));
  for (const char *const *pkgiter = pkgs; pkgiter && *pkgiter; pkgiter++)
    {
      auto pkg = *pkgiter;
      CXX_TRY_VAR (fds, rpmostreecxx::client_handle_fd_argument (pkg, basearch, is_replace), error);
      if (fds.size () > 0)
        {
          for (const auto &fd : fds)
            {
              auto idx = g_unix_fd_list_append (fd_list, fd, error);
              if (idx < 0)
                return glnx_prefix_error (error, "dup(%s)", pkg);
              g_variant_builder_add (&builder, "h", idx);
            }
        }
      else
        g_ptr_array_add (repo_pkgs, g_strdup (pkg));
    }

  *out_fd_idxs = g_variant_ref_sink (g_variant_new ("ah", &builder));
  *out_repo_pkgs = util::move_nullify (repo_pkgs);
  return TRUE;
}

static void
vardict_insert_strv (GVariantDict *dict, const char *key, const char *const *strv)
{
  if (strv && *strv)
    g_variant_dict_insert (dict, key, "^as", (char **)strv);
}

static gboolean
vardict_sort_and_insert_pkgs (GVariantDict *dict, const char *key_prefix, gboolean is_replace,
                              GUnixFDList *fd_list, const char *const *pkgs, GError **error)
{
  g_autoptr (GVariant) fd_idxs = NULL;
  g_autoptr (GPtrArray) repo_pkgs = NULL;

  if (!rpmostree_sort_pkgs_strv (pkgs, fd_list, is_replace, &repo_pkgs, &fd_idxs, error))
    return FALSE;

  /* for grep: here we insert
   * install-packages/install-fileoverride-packages/override-replace-packages */
  if (repo_pkgs != NULL && repo_pkgs->len > 0)
    g_variant_dict_insert_value (
        dict, glnx_strjoina (key_prefix, "-packages"),
        g_variant_new_strv ((const char *const *)repo_pkgs->pdata, repo_pkgs->len));

  /* for grep: here we insert
   * install-local-packages/install-fileoverride-local-packages/override-replace-local-packages */
  if (fd_idxs != NULL)
    g_variant_dict_insert_value (dict, glnx_strjoina (key_prefix, "-local-packages"), fd_idxs);
  return TRUE;
}

static gboolean
get_modifiers_variant (const char *set_refspec, const char *set_revision,
                       const char *const *install_pkgs,
                       const char *const *install_fileoverride_pkgs,
                       const char *const *uninstall_pkgs, const char *const *override_replace_pkgs,
                       const char *const *override_remove_pkgs,
                       const char *const *override_reset_pkgs, const char *local_repo_remote,
                       const char *treefile, GVariant **out_modifiers, GUnixFDList **out_fd_list,
                       GError **error)
{
  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_autoptr (GUnixFDList) fd_list = g_unix_fd_list_new ();

  if (install_pkgs)
    {
      if (!vardict_sort_and_insert_pkgs (&dict, "install", FALSE, fd_list, install_pkgs, error))
        return FALSE;
    }

  if (install_fileoverride_pkgs)
    {
      if (!vardict_sort_and_insert_pkgs (&dict, "install-fileoverride", FALSE, fd_list,
                                         install_fileoverride_pkgs, error))
        return FALSE;
    }

  if (override_replace_pkgs)
    {
      if (!vardict_sort_and_insert_pkgs (&dict, "override-replace", TRUE, fd_list,
                                         override_replace_pkgs, error))
        return FALSE;
    }

  if (set_refspec)
    g_variant_dict_insert (&dict, "set-refspec", "s", set_refspec);
  if (set_revision)
    g_variant_dict_insert (&dict, "set-revision", "s", set_revision);

  vardict_insert_strv (&dict, "uninstall-packages", uninstall_pkgs);
  vardict_insert_strv (&dict, "override-remove-packages", override_remove_pkgs);
  vardict_insert_strv (&dict, "override-reset-packages", override_reset_pkgs);

  if (local_repo_remote)
    {
      glnx_fd_close int repo_dfd = -1;
      if (!glnx_opendirat (AT_FDCWD, local_repo_remote, TRUE, &repo_dfd, error))
        return glnx_prefix_error (error, "Opening local repository as remote");

      int idx = g_unix_fd_list_append (fd_list, repo_dfd, error);
      if (idx < 0)
        return glnx_prefix_error (error, "Adding FD for local repository");

      g_variant_dict_insert (&dict, "ex-local-repo-remote", "h", idx);
    }

  if (treefile)
    g_variant_dict_insert (&dict, "treefile", "s", treefile);

  *out_fd_list = util::move_nullify (fd_list);
  *out_modifiers = g_variant_ref_sink (g_variant_dict_end (&dict));
  return TRUE;
}

gboolean
rpmostree_update_deployment (
    RPMOSTreeOS *os_proxy, const char *set_refspec, const char *set_revision,
    const char *const *install_pkgs, const char *const *install_fileoverride_pkgs,
    const char *const *uninstall_pkgs, const char *const *override_replace_pkgs,
    const char *const *override_remove_pkgs, const char *const *override_reset_pkgs,
    const char *local_repo_remote, const char *treefile, GVariant *options,
    char **out_transaction_address, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Updating deployment", error);
  g_autoptr (GVariant) modifiers = NULL;
  glnx_unref_object GUnixFDList *fd_list = NULL;
  if (!get_modifiers_variant (set_refspec, set_revision, install_pkgs, install_fileoverride_pkgs,
                              uninstall_pkgs, override_replace_pkgs, override_remove_pkgs,
                              override_reset_pkgs, local_repo_remote, treefile, &modifiers,
                              &fd_list, error))
    return glnx_prefix_error (error, "Preparing D-Bus arguments");

  return rpmostree_os_call_update_deployment_sync (
      os_proxy, modifiers, options, fd_list, out_transaction_address, NULL, cancellable, error);
}

static void
append_to_summary (GString *summary, const char *type, guint n)
{
  if (n == 0)
    return;
  if (summary->len > 0)
    g_string_append (summary, ", ");
  g_string_append_printf (summary, "%u %s", n, type);
}

static int
compare_sec_advisories (gconstpointer ap, gconstpointer bp)
{
  GVariant *a = *((GVariant **)ap);
  GVariant *b = *((GVariant **)bp);

  RpmOstreeAdvisorySeverity asev;
  g_variant_get_child (a, 2, "u", &asev);

  RpmOstreeAdvisorySeverity bsev;
  g_variant_get_child (b, 2, "u", &bsev);

  if (asev != bsev)
    return asev - bsev;

  const char *aid;
  g_variant_get_child (a, 0, "&s", &aid);

  const char *bid;
  g_variant_get_child (b, 0, "&s", &bid);

  return strcmp (aid, bid);
}

static const char *
severity_to_str (RpmOstreeAdvisorySeverity severity)
{
  switch (severity)
    {
    case RPM_OSTREE_ADVISORY_SEVERITY_LOW:
      return "Low";
    case RPM_OSTREE_ADVISORY_SEVERITY_MODERATE:
      return "Moderate";
    case RPM_OSTREE_ADVISORY_SEVERITY_IMPORTANT:
      return "Important";
    case RPM_OSTREE_ADVISORY_SEVERITY_CRITICAL:
      return "Critical";
    default: /* including NONE */
      return "Unknown";
    }
}

void
rpmostree_print_advisories (GVariant *advisories, gboolean verbose, guint max_key_len)
{
  /* counters for none/unknown, low, moderate, important, critical advisories */
  guint n_sev[RPM_OSTREE_ADVISORY_SEVERITY_LAST] = {
    0,
  };

  /* we only display security advisories for now */
  g_autoptr (GPtrArray) sec_advisories
      = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  guint max_id_len = 0;

  GVariantIter iter;
  g_variant_iter_init (&iter, advisories);
  while (TRUE)
    {
      g_autoptr (GVariant) child = g_variant_iter_next_value (&iter);
      if (!child)
        break;

      DnfAdvisoryKind kind;
      g_variant_get_child (child, 1, "u", &kind);

      /* we only display security advisories for now */
      if (kind != DNF_ADVISORY_KIND_SECURITY)
        continue;

      const char *id;
      g_variant_get_child (child, 0, "&s", &id);
      max_id_len = MAX (max_id_len, strlen (id));

      RpmOstreeAdvisorySeverity severity;
      g_variant_get_child (child, 2, "u", &severity);
      /* just make sure it's capped at LAST */
      if (severity < RPM_OSTREE_ADVISORY_SEVERITY_LAST)
        n_sev[severity]++;
      else /* bad val; count as unknown */
        n_sev[0]++;

      g_ptr_array_add (sec_advisories, g_variant_ref (child));
    }

  if (sec_advisories->len == 0)
    return;

  g_print ("%s%s", get_red_start (), get_bold_start ());

  /* this signals to just print leftmost */
  if (max_key_len == 0)
    g_print ("SecAdvisories:\n");
  else
    rpmostree_print_kv_no_newline ("SecAdvisories", max_key_len, "");

  if (!verbose)
    {
      /* just spell out "severity" for the unknown case, because e.g.
       * "SecAdvisories: 1 unknown" on its own is cryptic and scary */
      g_autoptr (GString) advisory_summary = g_string_new (NULL);
      const char *sev_str[] = { "unknown severity", "low", "moderate", "important", "critical" };
      g_assert_cmpint (G_N_ELEMENTS (n_sev), ==, G_N_ELEMENTS (sev_str));
      for (guint i = 0; i < G_N_ELEMENTS (sev_str); i++)
        append_to_summary (advisory_summary, sev_str[i], n_sev[i]);
      g_print ("%s\n", advisory_summary->str);
    }

  g_print ("%s%s", get_bold_end (), get_red_end ());
  if (!verbose)
    return;

  const guint max_sev_len = strlen ("Important");

  /* sort by severity */
  g_ptr_array_sort (sec_advisories, compare_sec_advisories);

  for (guint i = 0; i < sec_advisories->len; i++)
    {
      auto advisory = static_cast<GVariant *> (sec_advisories->pdata[i]);

      const char *id;
      g_variant_get_child (advisory, 0, "&s", &id);

      DnfAdvisoryKind kind;
      g_variant_get_child (advisory, 1, "u", &kind);

      RpmOstreeAdvisorySeverity severity;
      g_variant_get_child (advisory, 2, "u", &severity);

      g_autoptr (GVariant) pkgs = g_variant_get_child_value (advisory, 3);

      const char *severity_str = severity_to_str (severity);
      const guint n_pkgs = g_variant_n_children (pkgs);
      for (guint j = 0; j < n_pkgs; j++)
        {
          const char *nevra;
          g_variant_get_child (pkgs, j, "&s", &nevra);

          if (i == 0 && j == 0 && max_key_len > 0) /* we're on the same line as SecAdvisories */
            g_print ("%-*s  %-*s  %s\n", max_id_len, id, max_sev_len, severity_str, nevra);
          else
            g_print ("  %*s  %-*s  %-*s  %s\n", max_key_len, "", max_id_len, id, max_sev_len,
                     severity_str, nevra);
        }

      g_autoptr (GVariant) additional_info = g_variant_get_child_value (advisory, 4);
      g_auto (GVariantDict) dict;
      g_variant_dict_init (&dict, additional_info);

      g_autoptr (GVariant) refs
          = g_variant_dict_lookup_value (&dict, "cve_references", G_VARIANT_TYPE ("a(ss)"));

      /* for backwards compatibility with cached metadata from older versions */
      if (!refs)
        continue;

      const guint n_refs = g_variant_n_children (refs);
      for (guint j = 0; j < n_refs; j++)
        {
          const char *url, *title;
          g_variant_get_child (refs, j, "(&s&s)", &url, &title);
          g_print ("  %*s    %s\n", max_key_len, "", title);
          g_print ("  %*s    %s\n", max_key_len, "", url);
        }
    }
}

/* print "rpm-diff" and "advisories" GVariants from a cached update */
gboolean
rpmostree_print_diff_advisories (GVariant *rpm_diff, GVariant *advisories, gboolean verbose,
                                 gboolean verbose_advisories, guint max_key_len, GError **error)
{
  if (!rpm_diff)
    return TRUE; /* Nothing to 🖨️ */

  if (advisories)
    rpmostree_print_advisories (advisories, verbose || verbose_advisories, max_key_len);

  g_auto (GVariantDict) rpm_diff_dict;
  g_variant_dict_init (&rpm_diff_dict, rpm_diff);

  g_autoptr (GVariant) upgraded = _rpmostree_vardict_lookup_value_required (
      &rpm_diff_dict, "upgraded", RPMOSTREE_DIFF_MODIFIED_GVARIANT_FORMAT, error);
  if (!upgraded)
    return FALSE;

  g_autoptr (GVariant) downgraded = _rpmostree_vardict_lookup_value_required (
      &rpm_diff_dict, "downgraded", RPMOSTREE_DIFF_MODIFIED_GVARIANT_FORMAT, error);
  if (!downgraded)
    return FALSE;

  g_autoptr (GVariant) removed = _rpmostree_vardict_lookup_value_required (
      &rpm_diff_dict, "removed", RPMOSTREE_DIFF_SINGLE_GVARIANT_FORMAT, error);
  if (!removed)
    return FALSE;

  g_autoptr (GVariant) added = _rpmostree_vardict_lookup_value_required (
      &rpm_diff_dict, "added", RPMOSTREE_DIFF_SINGLE_GVARIANT_FORMAT, error);
  if (!added)
    return FALSE;

  if (verbose)
    rpmostree_variant_diff_print_formatted (max_key_len, upgraded, downgraded, removed, added);
  else
    {
      g_autofree char *diff_summary = rpmostree_generate_diff_summary (
          g_variant_n_children (upgraded), g_variant_n_children (downgraded),
          g_variant_n_children (removed), g_variant_n_children (added));
      if (strlen (diff_summary) > 0) /* only print if we have something to print */
        rpmostree_print_kv ("Diff", max_key_len, diff_summary);
    }

  return TRUE;
}

/* print "manifest-diff" */
gboolean
rpmostree_print_manifest_diff (GVariant *manifest_diff, guint maxkeylen, GError **error)
{
  if (!manifest_diff)
    return TRUE; /* Nothing to 🖨️ */

  g_auto (GVariantDict) manifest_diff_dict;
  g_variant_dict_init (&manifest_diff_dict, manifest_diff);

  guint64 total = 0;
  if (!g_variant_dict_lookup (&manifest_diff_dict, "total", "t", &total))
    return glnx_throw (error, "Missing \"total\" key");

  printf ("  %*s%s %" G_GUINT64_FORMAT "\n", maxkeylen, "Total layers",
          strlen ("Total layers") ? ":" : " ", total);

  {
    guint64 total_size = 0;
    if (!g_variant_dict_lookup (&manifest_diff_dict, "total-size", "t", &total_size))
      return glnx_throw (error, "Missing \"total-size\" key");
    g_autofree char *formatted = g_format_size (total_size);
    printf ("  %*s%s %s", maxkeylen, "Size", strlen ("Size") ? ":" : " ", formatted);
    printf ("\n");
  }

  guint64 total_removed = 0;
  if (!g_variant_dict_lookup (&manifest_diff_dict, "n-removed", "t", &total_removed))
    return glnx_throw (error, "Missing \"n-removed\" key");

  printf (" %*s%s %" G_GUINT64_FORMAT "\n", maxkeylen, "Removed layers",
          strlen ("Removed layers") ? ":" : " ", total_removed);

  {
    guint64 removed_size = 0;
    if (!g_variant_dict_lookup (&manifest_diff_dict, "removed-size", "t", &removed_size))
      return glnx_throw (error, "Missing \"removed-size\" key");
    g_autofree char *formatted = g_format_size (removed_size);
    printf ("  %*s%s %s\n", maxkeylen, "Size", strlen ("Size") ? ":" : " ", formatted);
  }

  guint64 total_added = 0;
  if (!g_variant_dict_lookup (&manifest_diff_dict, "n-added", "t", &total_added))
    return glnx_throw (error, "Missing \"n-added\" key");
  printf ("  %*s%s %" G_GUINT64_FORMAT "\n", maxkeylen, "Added layers",
          strlen ("Added layers") ? ":" : " ", total_added);

  {
    guint64 added_size = 0;
    if (!g_variant_dict_lookup (&manifest_diff_dict, "added-size", "t", &added_size))
      return glnx_throw (error, "Missing \"added-size\" key");

    g_autofree char *formatted = g_format_size (added_size);
    printf ("  %*s%s %s\n", maxkeylen, "Size", strlen ("Size") ? ":" : " ", formatted);
  }

  return TRUE;
}

/* this is used by both `status` and `upgrade --check/--preview` */
gboolean
rpmostree_print_cached_update (GVariant *cached_update, gboolean verbose,
                               gboolean verbose_advisories, GCancellable *cancellable,
                               GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Retrieving cached update", error);

  g_auto (GVariantDict) dict;
  g_variant_dict_init (&dict, cached_update);

  /* let's just extract 📤 all the info ahead of time */

  const char *checksum;
  if (!g_variant_dict_lookup (&dict, "checksum", "&s", &checksum))
    return glnx_throw (error, "Missing \"checksum\" key");

  const char *version;
  if (!g_variant_dict_lookup (&dict, "version", "&s", &version))
    version = NULL;

  g_autofree char *timestamp = NULL;
  {
    guint64 t;
    if (!g_variant_dict_lookup (&dict, "timestamp", "t", &t))
      t = 0;
    timestamp = rpmostree_timestamp_str_from_unix_utc (t);
  }

  gboolean gpg_enabled;
  if (!g_variant_dict_lookup (&dict, "gpg-enabled", "b", &gpg_enabled))
    gpg_enabled = FALSE;

  g_autoptr (GVariant) signatures
      = g_variant_dict_lookup_value (&dict, "signatures", G_VARIANT_TYPE ("av"));

  gboolean is_new_checksum;
  g_assert (g_variant_dict_lookup (&dict, "ref-has-new-commit", "b", &is_new_checksum));

  g_autoptr (GVariant) rpm_diff
      = g_variant_dict_lookup_value (&dict, "rpm-diff", G_VARIANT_TYPE ("a{sv}"));

  g_autoptr (GVariant) manifest_diff
      = g_variant_dict_lookup_value (&dict, "manifest-diff", G_VARIANT_TYPE ("a{sv}"));

  g_autoptr (GVariant) advisories
      = g_variant_dict_lookup_value (&dict, "advisories", G_VARIANT_TYPE ("a(suuasa{sv})"));

  /* and now we can print 🖨️ things! */

  g_print ("AvailableUpdate:\n");

  /* add the long keys here */
  const guint max_key_len = MAX (strlen ("SecAdvisories"), strlen ("GPGSignature"));

  if (is_new_checksum)
    {
      rpmostree_print_timestamp_version (version, timestamp, max_key_len);
      rpmostree_print_kv ("Commit", max_key_len, checksum);
      if (gpg_enabled)
        rpmostree_print_gpg_info (signatures, verbose, max_key_len);
    }

  if (!rpmostree_print_diff_advisories (rpm_diff, advisories, verbose, verbose_advisories,
                                        max_key_len, error))
    return FALSE;

  if (!rpmostree_print_manifest_diff (manifest_diff, max_key_len, error))
    return FALSE;

  return TRUE;
}

/* Query systemd for systemd unit's object path using method_name provided with
 * parameters. The reply_type of method_name must be G_VARIANT_TYPE_TUPLE. */
gboolean
get_sd_unit_objpath (GDBusConnection *connection, const char *method_name, GVariant *parameters,
                     const char **unit_objpath, GCancellable *cancellable, GError **error)
{
  g_autoptr (GVariant) update_driver_objpath_tuple = g_dbus_connection_call_sync (
      connection, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
      "org.freedesktop.systemd1.Manager", method_name, parameters, G_VARIANT_TYPE_TUPLE,
      G_DBUS_CALL_FLAGS_NONE, -1, cancellable, error);
  if (!update_driver_objpath_tuple)
    return FALSE;
  else if (g_variant_n_children (update_driver_objpath_tuple) < 1)
    return glnx_throw (error, "%s returned empty tuple", method_name);

  g_autoptr (GVariant) update_driver_objpath_val
      = g_variant_get_child_value (update_driver_objpath_tuple, 0);
  *unit_objpath = g_variant_dup_string (update_driver_objpath_val, NULL);
  g_assert (*unit_objpath);

  return TRUE;
}

/* Get the property_name property of sd_unit. Returns a reference to the GVariant
 * instance that holds the value for property_name GVariant in cache_val. */
static gboolean
get_sd_unit_property (GDBusConnection *connection, const char *method_name, GVariant *parameters,
                      const char *property_name, GVariant **cache_val, GCancellable *cancellable,
                      GError **error)
{
  g_autofree const char *objpath = NULL;
  if (!get_sd_unit_objpath (connection, method_name, parameters, &objpath, cancellable, error))
    return FALSE;

  /* Look up property_name property of systemd unit. */
  g_autoptr (GDBusProxy) unit_obj_proxy = g_dbus_proxy_new_sync (
      connection, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL, "org.freedesktop.systemd1",
      objpath, "org.freedesktop.systemd1.Unit", cancellable, error);
  if (!unit_obj_proxy)
    return FALSE;

  *cache_val = g_dbus_proxy_get_cached_property (unit_obj_proxy, property_name);
  if (!*cache_val)
    return glnx_throw (error, "%s property not found in proxy's cache (%s)", property_name,
                       objpath);

  return TRUE;
}

/* Helper function to append docs information for sd_unit to str.
 * Prints any errors that occur. */
static void
append_docs_to_str (GString *str, GDBusConnection *connection, char *sd_unit,
                    GCancellable *cancellable)
{
  g_autoptr (GVariant) docs_array = NULL;
  g_autoptr (GError) local_error = NULL;
  if (!get_sd_unit_property (connection, "LoadUnit", g_variant_new ("(s)", sd_unit),
                             "Documentation", &docs_array, cancellable, &local_error))
    {
      g_printerr ("%s", local_error->message);
    }
  else if (docs_array)
    {
      gsize docs_len;
      g_autofree const char **docs = g_variant_get_strv (docs_array, &docs_len);
      if (docs_len > 0)
        {
          g_string_append_printf (str, " at ");
          for (guint i = 0; i < docs_len; i++)
            g_string_append_printf (str, "%s%s", docs[i], i < docs_len - 1 ? ", " : "\n");
        }
      else
        {
          g_string_append_printf (str, "\n");
        }
    }
}

/* Check whether sd_unit's `ActiveState` is "active" and return the boolean
 * in `sd_unit_is_active`. */
static gboolean
check_sd_unit_state_is_active (char *sd_unit, gboolean *sd_unit_is_active,
                               GDBusConnection *connection, GCancellable *cancellable,
                               GError **error)
{
  g_autoptr (GVariant) active_state = NULL;
  if (!get_sd_unit_property (connection, "LoadUnit", g_variant_new ("(s)", sd_unit), "ActiveState",
                             &active_state, cancellable, error))
    return FALSE;
  g_assert (active_state);
  // NB: include "failed" in states we consider "active" so we do not ignore crashed
  // updates drivers.
  const char *states[] = { "active", "activating", "reloading", "failed", NULL };
  const char *active_state_str = g_variant_get_string (active_state, NULL);
  *sd_unit_is_active = (active_state_str && g_strv_contains (states, active_state_str));
  return TRUE;
}

/* Check whether sd_unit contains pid and return the boolean in sd_unit_contains_pid. */
static gboolean
check_sd_unit_contains_pid (char *sd_unit, pid_t pid, gboolean *sd_unit_contains_pid,
                            GDBusConnection *connection, GCancellable *cancellable, GError **error)
{
  // Get the systemd unit associated with pid.
  g_autoptr (GVariant) process_sd_unit_val = NULL;
  const char *process_sd_unit = NULL;
  if (!get_sd_unit_property (connection, "GetUnitByPID", g_variant_new ("(u)", (guint32)pid), "Id",
                             &process_sd_unit_val, cancellable, error))
    return FALSE;
  process_sd_unit = g_variant_get_string (process_sd_unit_val, NULL);
  if (g_strcmp0 (process_sd_unit, sd_unit) == 0)
    *sd_unit_contains_pid = TRUE;
  else
    *sd_unit_contains_pid = FALSE;
  return TRUE;
}

/* Throw an error if an updates driver is registered and active. */
gboolean
error_if_driver_registered (RPMOSTreeSysroot *sysroot_proxy, GCancellable *cancellable,
                            GError **error)
{
  g_autofree char *update_driver_sd_unit = NULL;
  g_autofree char *update_driver_name = NULL;
  if (!get_driver_info (&update_driver_name, &update_driver_sd_unit, error))
    return FALSE;

  /* Throw an error if an updates driver is registered since deployments should be
   * done through the driver. */
  if (update_driver_sd_unit && update_driver_name)
    {
      GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));

      // Do not error out if current process' systemd unit is the same as updates driver's.
      pid_t pid = getpid ();
      gboolean sd_unit_contains_pid = FALSE;
      if (!check_sd_unit_contains_pid (update_driver_sd_unit, pid, &sd_unit_contains_pid,
                                       connection, cancellable, error))
        return FALSE;
      if (sd_unit_contains_pid)
        return TRUE;

      // Build and throw error message.
      g_autoptr (GString) error_msg = g_string_new (NULL);
      g_string_printf (error_msg, "Updates and deployments are driven by %s (%s)\n",
                       update_driver_name, update_driver_sd_unit);
      g_string_append_printf (error_msg, "See %s's documentation", update_driver_name);
      // Only try to get unit's `Documentation` and check unit's state if we're on the system bus.
      gboolean sd_unit_is_active = TRUE;
      if (!check_sd_unit_state_is_active (update_driver_sd_unit, &sd_unit_is_active, connection,
                                          cancellable, error))
        return FALSE;
      // Ignore driver if driver's `ActiveState` is not "active", even if registered.
      if (!sd_unit_is_active)
        return TRUE;
      append_docs_to_str (error_msg, connection, update_driver_sd_unit, cancellable);
      g_string_append_printf (error_msg,
                              "Use --bypass-driver to bypass %s and perform the operation anyways",
                              update_driver_name);
      return glnx_throw (error, "%s", error_msg->str);
    }

  return TRUE;
}
