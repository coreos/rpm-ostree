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

#include "rpmostree-dbus-helpers.h"
#include "libgsystem.h"
#include "libglnx.h"
#include <sys/socket.h>
#include "glib-unix.h"
#include <signal.h>

static GPid peer_pid = 0;

void
rpmostree_cleanup_peer ()
{
  if (peer_pid > 0)
    kill (peer_pid, SIGTERM);
}

static gboolean
get_connection_for_path (gchar *sysroot,
                         gboolean force_peer,
                         GCancellable *cancellable,
                         GDBusConnection **out_connection,
                         GError **error)
{
  glnx_unref_object GDBusConnection *connection = NULL;
  glnx_unref_object GDBusObjectManager *om = NULL;
  glnx_unref_object GSocketConnection *stream = NULL;
  glnx_unref_object GSocket *socket = NULL;

  gchar buffer[16];

  int pair[2];
  gboolean ret = FALSE;

  const gchar *args[] = {
    "rpm-ostreed",
    "--sysroot", sysroot,
    "--dbus-peer", buffer,
    NULL
  };

  if (!sysroot)
    sysroot = "/";

  if (g_strcmp0 ("/", sysroot) == 0 && force_peer == FALSE)
    {
      connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, error);
      goto out;
    }

  g_print ("Running in single user mode. Be sure no other users are modifying the system\n");
  if (socketpair (AF_UNIX, SOCK_STREAM, 0, pair) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't create socket pair: %s",
                   g_strerror (errno));
      goto out;
    }

  g_snprintf (buffer, sizeof (buffer), "%d", pair[1]);

  socket = g_socket_new_from_fd (pair[0], error);
  if (socket == NULL)
    {
      close (pair[0]);
      close (pair[1]);
      goto out;
    }

  if (!g_spawn_async (NULL, (gchar **)args, NULL,
                      G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_DO_NOT_REAP_CHILD,
                      NULL, NULL, &peer_pid, error))
    {
      close (pair[1]);
      goto out;
    }

  stream = g_socket_connection_factory_create_connection (socket);
  connection = g_dbus_connection_new_sync (G_IO_STREAM (stream), NULL,
                                           G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                           NULL, cancellable, error);

out:
  if (connection)
    {
      ret = TRUE;
      *out_connection = g_steal_pointer (&connection);
    }
  return ret;
}


/**
* rpmostree_load_sysroot
* @sysroot: sysroot path
* @force_peer: Force a peer connection
* @cancellable: A GCancellable
* @out_sysroot: (out) Return location for sysroot
* @error: A pointer to a GError pointer.
*
* Returns: True on success
**/
gboolean
rpmostree_load_sysroot (gchar *sysroot,
                        gboolean force_peer,
                        GCancellable *cancellable,
                        RPMOSTreeSysroot **out_sysroot_proxy,
                        GError **error)
{
  gboolean ret = FALSE;
  const char *bus_name = NULL;
  glnx_unref_object GDBusConnection *connection = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  if (!get_connection_for_path (sysroot,
                                force_peer,
                                cancellable,
                                &connection,
                                error))
    goto out;

  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  sysroot_proxy = rpmostree_sysroot_proxy_new_sync (connection,
                                                    G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                    bus_name,
                                                    "/org/projectatomic/rpmostree1/Sysroot",
                                                    NULL,
                                                    error);
  if (sysroot_proxy == NULL)
    goto out;

  *out_sysroot_proxy = g_steal_pointer (&sysroot_proxy);
  ret = TRUE;

out:
  return ret;
}

gboolean
rpmostree_load_os_proxy (RPMOSTreeSysroot *sysroot_proxy,
                         gchar *opt_osname,
                         GCancellable *cancellable,
                         RPMOSTreeOS **out_os_proxy,
                         GError **error)
{
  gboolean ret = FALSE;
  const char *bus_name;
  g_autofree char *os_object_path = NULL;
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;

  GDBusConnection *connection = NULL; /* owned by sysroot_proxy */

  if (opt_osname == NULL)
    {
      os_object_path = rpmostree_sysroot_dup_booted (sysroot_proxy);
    }

  if (os_object_path == NULL)
    {
      /* Usually if opt_osname is null and the property isn't
         populated that means the daemon isn't listen on the bus
         make the call anyways to get the standard error.
      */
      if (!opt_osname)
        opt_osname = "";

      if (!rpmostree_sysroot_call_get_os_sync (sysroot_proxy,
                                               opt_osname,
                                               &os_object_path,
                                               cancellable,
                                               error))
        goto out;
    }

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));

  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  os_proxy = rpmostree_os_proxy_new_sync (connection,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          bus_name,
                                          os_object_path,
                                          cancellable,
                                          error);

  if (os_proxy == NULL)
    goto out;

  *out_os_proxy = g_steal_pointer (&os_proxy);
  ret = TRUE;

out:
  return ret;
}


/**
* transaction_console_get_progress_line
*
* Similar to ostree_repo_pull_default_console_progress_changed
*
* Displays outstanding fetch progress in bytes/sec,
* or else outstanding content or metadata writes to the repository in
* number of objects.
**/
static gchar *
transaction_get_progress_line (guint64 start_time,
                               guint64 elapsed_secs,
                               guint outstanding_fetches,
                               guint outstanding_writes,
                               guint n_scanned_metadata,
                               guint metadata_fetched,
                               guint outstanding_metadata_fetches,
                               guint total_delta_parts,
                               guint fetched_delta_parts,
                               guint total_delta_superblocks,
                               guint64 total_delta_part_size,
                               guint fetched,
                               guint requested,
                               guint64 bytes_transferred,
                               guint64 bytes_sec)
{
  GString *buf;

  buf = g_string_new ("");

  if (outstanding_fetches)
    {
      g_autofree gchar *formatted_bytes_transferred = g_format_size_full (bytes_transferred, 0);
      g_autofree gchar *formatted_bytes_sec = NULL;

      if (!bytes_sec)
        formatted_bytes_sec = g_strdup ("-");
      else
        formatted_bytes_sec = g_format_size (bytes_sec);

      if (total_delta_parts > 0)
        {
          g_autofree gchar *formatted_total = g_format_size (total_delta_part_size);
          g_string_append_printf (buf, "Receiving delta parts: %u/%u %s/s %s/%s",
                                  fetched_delta_parts, total_delta_parts,
                                  formatted_bytes_sec, formatted_bytes_transferred,
                                  formatted_total);
        }
      else if (outstanding_metadata_fetches)
        {
          g_string_append_printf (buf, "Receiving metadata objects: %u/(estimating) %s/s %s",
                                  metadata_fetched, formatted_bytes_sec, formatted_bytes_transferred);
        }
      else
        {
          g_string_append_printf (buf, "Receiving objects: %u%% (%u/%u) %s/s %s",
                                  (guint)((((double)fetched) / requested) * 100),
                                  fetched, requested, formatted_bytes_sec, formatted_bytes_transferred);
        }
    }
  else if (outstanding_writes)
    {
      g_string_append_printf (buf, "Writing objects: %u", outstanding_writes);
    }
  else
    {
      g_string_append_printf (buf, "Scanning metadata: %u", n_scanned_metadata);
    }

  return g_string_free (buf, FALSE);
}


typedef struct
{
  GSConsole *console;
  gboolean in_status_line;
  GError *error;
  GMainLoop *loop;
  gboolean complete;
} TransactionProgress;


static TransactionProgress *
transaction_progress_new (void)
{
  TransactionProgress *self;

  self = g_slice_new0 (TransactionProgress);
  self->console = gs_console_get ();
  self->loop = g_main_loop_new (NULL, FALSE);

  return self;
}


static void
transaction_progress_free (TransactionProgress *self)
{
  g_main_loop_unref (self->loop);
  g_slice_free (TransactionProgress, self);
}


static gboolean
end_status_line (TransactionProgress *self)
{
  gboolean ret = TRUE;

  if (self->in_status_line)
    {
      ret = gs_console_end_status_line (self->console, NULL, NULL);
      self->in_status_line = FALSE;
    }

  return ret;
}


static gboolean
add_status_line (TransactionProgress *self,
                 const char *line)
{
  self->in_status_line = TRUE;
  return gs_console_begin_status_line (self->console, line, NULL, NULL);
}


static void
transaction_progress_end (TransactionProgress *self)
{
  end_status_line (self);
  self->console = NULL;
  g_main_loop_quit (self->loop);
}


static void
on_transaction_progress (GDBusProxy *proxy,
                         gchar *sender_name,
                         gchar *signal_name,
                         GVariant *parameters,
                         gpointer user_data)
{
  TransactionProgress *tp = user_data;

  if (g_strcmp0 (signal_name, "SignatureProgress") == 0)
    {
      g_autoptr(GVariant) sig = NULL;
      sig = g_variant_get_child_value (parameters, 0);
      rpmostree_print_signatures (g_variant_ref (sig), "  ");
      add_status_line (tp, "\n");
    }
  else if (g_strcmp0 (signal_name, "Message") == 0)
    {
      g_autofree gchar *message = NULL;

      g_variant_get_child (parameters, 0, "s", &message);
      if (tp->in_status_line)
        add_status_line (tp, message);
      else
        g_print ("%s\n", message);
    }
  else if (g_strcmp0 (signal_name, "DownloadProgress") == 0)
    {
      g_autofree gchar *line = NULL;

      guint64 start_time;
      guint64 elapsed_secs;
      guint outstanding_fetches;
      guint outstanding_writes;
      guint n_scanned_metadata;
      guint metadata_fetched;
      guint outstanding_metadata_fetches;
      guint total_delta_parts;
      guint fetched_delta_parts;
      guint total_delta_superblocks;
      guint64 total_delta_part_size;
      guint fetched;
      guint requested;
      guint64 bytes_transferred;
      guint64 bytes_sec;
      g_variant_get (parameters, "((tt)(uu)(uuu)(uuut)(uu)(tt))",
                     &start_time, &elapsed_secs,
                     &outstanding_fetches, &outstanding_writes,
                     &n_scanned_metadata, &metadata_fetched,
                     &outstanding_metadata_fetches,
                     &total_delta_parts, &fetched_delta_parts,
                     &total_delta_superblocks, &total_delta_part_size,
                     &fetched, &requested, &bytes_transferred, &bytes_sec);

      line = transaction_get_progress_line (start_time, elapsed_secs,
                                            outstanding_fetches,
                                            outstanding_writes,
                                            n_scanned_metadata,
                                            metadata_fetched,
                                            outstanding_metadata_fetches,
                                            total_delta_parts,
                                            fetched_delta_parts,
                                            total_delta_superblocks,
                                            total_delta_part_size,
                                            fetched,
                                            requested,
                                            bytes_transferred,
                                            bytes_sec);
      add_status_line (tp, line);
    }
  else if (g_strcmp0 (signal_name, "ProgressEnd") == 0)
    {
      end_status_line (tp);
    }
}

static void
on_owner_changed (GObject    *object,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  /* Owner shouldn't change durning a transaction
   * that messes with notifications, abort, abort.
   */
  TransactionProgress *tp = user_data;
  tp->error = g_dbus_error_new_for_dbus_error ("org.projectatomic.rpmostreed.Error.Failed",
                                               "Bus owner changed, aborting.");
  transaction_progress_end (tp);
}

static void
transaction_done (RPMOSTreeTransaction *transaction,
                  TransactionProgress *tp)
{
  g_autofree gchar *message = NULL;
  gboolean success = FALSE;

  /* if we are already finished don't process */
  if (tp->complete)
    return;

  tp->complete = TRUE;

  if (!tp->error)
    {
      if (rpmostree_transaction_call_finish_sync (transaction,
                                                  &success, &message,
                                                  NULL, &tp->error))
        {
          if (success)
            {
              add_status_line (tp, message);
            }
          else
            {
              tp->error = g_dbus_error_new_for_dbus_error ("org.projectatomic.rpmostreed.Error.Failed",
                                                           message);
            }
        }
    }

  transaction_progress_end (tp);
}

static void
on_transaction_done (RPMOSTreeTransaction *transaction,
                     GParamSpec *pspec,
                     TransactionProgress *tp)
{
  if (!rpmostree_transaction_get_active (transaction))
    transaction_done (transaction, tp);
}

static void
cancelled_handler (GCancellable *cancellable,
                   gpointer user_data)
{
  RPMOSTreeTransaction *transaction = user_data;
  rpmostree_transaction_call_cancel_sync (transaction, NULL, NULL);
}


gboolean
rpmostree_transaction_get_response_sync (RPMOSTreeSysroot *sysroot_proxy,
                                         const gchar *object_path,
                                         GCancellable *cancellable,
                                         GError **error)
{
  GDBusConnection *connection;
  glnx_unref_object GDBusObjectManager *object_manager = NULL;
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;

  TransactionProgress *tp = transaction_progress_new ();

  const char *bus_name;
  gint cancel_handler;
  gulong property_handler = 0;
  gulong signal_handler = 0;
  gboolean success = FALSE;

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));

  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  /* If we are on the message bus, setup object manager connection
   * to notify if the owner changes. */
  if (bus_name != NULL)
    {
      object_manager = rpmostree_object_manager_client_new_sync (connection,
                                                          G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                          bus_name,
                                                          "/org/projectatomic/rpmostree1",
                                                          cancellable,
                                                          error);

      if (object_manager == NULL)
        goto out;

      g_signal_connect (object_manager,
                        "notify::name-owner",
                        G_CALLBACK (on_owner_changed),
                        tp);
    }

  transaction = rpmostree_transaction_proxy_new_sync (connection,
                                                      G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                      bus_name,
                                                      object_path,
                                                      cancellable,
                                                      error);
  if (transaction == NULL)
    goto out;

  /* setup cancel handler */
  cancel_handler = g_cancellable_connect (cancellable,
                                          G_CALLBACK (cancelled_handler),
                                          transaction, NULL);

  signal_handler = g_signal_connect (transaction, "g-signal",
                                     G_CALLBACK (on_transaction_progress),
                                     tp);

  /* Setup finished signal handlers */
  property_handler = g_signal_connect (transaction, "notify::active",
                                       G_CALLBACK (on_transaction_done),
                                       tp);

  if (rpmostree_transaction_get_active (transaction))
    g_main_loop_run (tp->loop);
  else
    transaction_done (transaction, tp);

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

out:
  if (property_handler)
    g_signal_handler_disconnect (transaction, property_handler);

  if (signal_handler)
    g_signal_handler_disconnect (transaction, signal_handler);

  transaction_progress_free (tp);
  return success;
}


void
rpmostree_print_signatures (GVariant *variant,
                            const gchar *sep)
{
  GString *sigs_buffer;
  guint i;
  guint n_sigs = g_variant_n_children (variant);
  sigs_buffer = g_string_sized_new (256);

  for (i = 0; i < n_sigs; i++)
    {
      g_autoptr(GVariant) v = NULL;
      g_string_append_c (sigs_buffer, '\n');
      g_variant_get_child (variant, i, "v", &v);
      ostree_gpg_verify_result_describe_variant (v, sigs_buffer, sep,
                                                 OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
    }

  g_print ("%s", sigs_buffer->str);
  g_string_free (sigs_buffer, TRUE);
}
