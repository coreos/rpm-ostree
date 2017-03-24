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
#include "libglnx.h"
#include <sys/socket.h>
#include "glib-unix.h"
#include <signal.h>

void
rpmostree_cleanup_peer (GPid *peer_pid)
{
  if (*peer_pid > 0)
    kill (*peer_pid, SIGTERM);
}

static GDBusConnection*
get_connection_for_path (gchar *sysroot,
                         gboolean force_peer,
                         GPid *out_peer_pid,
                         GCancellable *cancellable,
                         GError **error)
{
  glnx_unref_object GDBusConnection *connection = NULL;
  glnx_unref_object GSocketConnection *stream = NULL;
  glnx_unref_object GSocket *socket = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  gchar buffer[16];

  int pair[2];

  const gchar *args[] = {
    "rpm-ostree",
    "start-daemon",
    "--sysroot", sysroot,
    "--dbus-peer", buffer,
    NULL
  };

  /* This is only intended for use by installed tests.
   * Note that it disregards the 'sysroot' and 'force_peer' options
   * and assumes the service activation command has been configured
   * to use the desired system root path. */
  if (g_getenv ("RPMOSTREE_USE_SESSION_BUS") != NULL)
    {
      if (sysroot != NULL)
        g_warning ("RPMOSTREE_USE_SESSION_BUS set, ignoring --sysroot=%s", sysroot);

      /* NB: as opposed to other early returns, this is _also_ a happy path */
      return g_bus_get_sync (G_BUS_TYPE_SESSION, cancellable, error);
    }

  if (sysroot == NULL)
    sysroot = "/";

  if (g_strcmp0 ("/", sysroot) == 0 && force_peer == FALSE)
    /* NB: as opposed to other early returns, this is _also_ a happy path */
    return g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, error);

  g_print ("Running in single user mode. Be sure no other users are modifying the system\n");
  if (socketpair (AF_UNIX, SOCK_STREAM, 0, pair) < 0)
    return glnx_null_throw_errno_prefix (error, "couldn't create socket pair");

  g_snprintf (buffer, sizeof (buffer), "%d", pair[1]);

  socket = g_socket_new_from_fd (pair[0], error);
  if (socket == NULL)
    {
      close (pair[0]);
      close (pair[1]);
      return NULL;
    }

  if (!g_spawn_async (NULL, (gchar **)args, NULL,
                      G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_DO_NOT_REAP_CHILD,
                      NULL, NULL, &peer_pid, error))
    {
      close (pair[1]);
      return NULL;
    }

  stream = g_socket_connection_factory_create_connection (socket);
  connection = g_dbus_connection_new_sync (G_IO_STREAM (stream), NULL,
                                           G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                           NULL, cancellable, error);
  if (!connection)
    return NULL;

  *out_peer_pid = peer_pid; peer_pid = 0;
  return connection;
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
                        GPid *out_peer_pid,
                        GError **error)
{
  const char *bus_name = NULL;
  glnx_unref_object GDBusConnection *connection = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GVariantBuilder) options_builder =
    g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  _cleanup_peer_ GPid peer_pid = 0;

  connection = get_connection_for_path (sysroot, force_peer, &peer_pid,
                                        cancellable, error);
  if (connection == NULL)
    return FALSE;

  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  sysroot_proxy = rpmostree_sysroot_proxy_new_sync (connection,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    bus_name,
                                                    "/org/projectatomic/rpmostree1/Sysroot",
                                                    NULL,
                                                    error);
  if (sysroot_proxy == NULL)
    return FALSE;

  /* This tells the daemon not to auto-exit as long as we are alive */
  if (!rpmostree_sysroot_call_register_client_sync (sysroot_proxy,
                                                    g_variant_builder_end (options_builder),
                                                    cancellable, error))
    return FALSE;

  *out_sysroot_proxy = g_steal_pointer (&sysroot_proxy);
  *out_peer_pid = peer_pid; peer_pid = 0;
  return TRUE;
}

gboolean
rpmostree_load_os_proxies (RPMOSTreeSysroot *sysroot_proxy,
                           gchar *opt_osname,
                           GCancellable *cancellable,
                           RPMOSTreeOS **out_os_proxy,
                           RPMOSTreeOSExperimental **out_osexperimental_proxy,
                           GError **error)
{
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

      if (!rpmostree_sysroot_call_get_os_sync (sysroot_proxy,
                                               opt_osname,
                                               &os_object_path,
                                               cancellable,
                                               error))
        return FALSE;
    }

  /* owned by sysroot_proxy */
  GDBusConnection *connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot_proxy));
  const char *bus_name;
  if (g_dbus_connection_get_unique_name (connection) != NULL)
    bus_name = BUS_NAME;

  glnx_unref_object RPMOSTreeOS *os_proxy =
    rpmostree_os_proxy_new_sync (connection,
                                 G_DBUS_PROXY_FLAGS_NONE,
                                 bus_name,
                                 os_object_path,
                                 cancellable,
                                 error);
  if (os_proxy == NULL)
    return FALSE;

  glnx_unref_object RPMOSTreeOSExperimental *ret_osexperimental_proxy = NULL;
  if (out_osexperimental_proxy)
    {
      ret_osexperimental_proxy =
        rpmostree_osexperimental_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 bus_name,
                                                 os_object_path,
                                                 cancellable,
                                                 error);
      if (!ret_osexperimental_proxy)
        return FALSE;
    }

  *out_os_proxy = g_steal_pointer (&os_proxy);
  if (out_osexperimental_proxy)
    *out_osexperimental_proxy = g_steal_pointer (&ret_osexperimental_proxy);
  return TRUE;
}

gboolean
rpmostree_load_os_proxy (RPMOSTreeSysroot *sysroot_proxy,
                         gchar *opt_osname,
                         GCancellable *cancellable,
                         RPMOSTreeOS **out_os_proxy,
                         GError **error)
{
  return rpmostree_load_os_proxies (sysroot_proxy, opt_osname, cancellable,
                                    out_os_proxy, NULL, error);
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
  GLnxConsoleRef console;
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
  self->loop = g_main_loop_new (NULL, FALSE);

  return self;
}


static void
transaction_progress_free (TransactionProgress *self)
{
  g_main_loop_unref (self->loop);
  g_slice_free (TransactionProgress, self);
}


static void
end_status_line (TransactionProgress *self)
{
  if (self->in_status_line)
    {
      glnx_console_unlock (&self->console);
      self->in_status_line = FALSE;
    }
}


static void
add_status_line (TransactionProgress *self,
                 const char *line,
                 int percentage)
{
  self->in_status_line = TRUE;
  if (!self->console.locked)
    glnx_console_lock (&self->console);
  if (percentage < 0)
    glnx_console_text (line);
  else
    glnx_console_progress_text_percent (line, percentage);
}


static void
transaction_progress_end (TransactionProgress *self)
{
  end_status_line (self);
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
      add_status_line (tp, "\n", -1);
    }
  else if (g_strcmp0 (signal_name, "Message") == 0)
    {
      const gchar *message = NULL;
      g_variant_get_child (parameters, 0, "&s", &message);
      if (tp->in_status_line)
        add_status_line (tp, message, -1);
      else
        g_print ("%s\n", message);
    }
  else if (g_strcmp0 (signal_name, "TaskBegin") == 0)
    {
      /* XXX: whenever libglnx implements a spinner, this would be appropriate
       * here. */
      const gchar *message = NULL;
      g_variant_get_child (parameters, 0, "&s", &message);
      g_print ("%s... ", message);
    }
  else if (g_strcmp0 (signal_name, "TaskEnd") == 0)
    {
      const gchar *message = NULL;
      g_variant_get_child (parameters, 0, "&s", &message);
      g_print ("%s\n", message);
    }
  else if (g_strcmp0 (signal_name, "PercentProgress") == 0)
    {
      const gchar *message = NULL;
      guint32 percentage;
      g_variant_get_child (parameters, 0, "&s", &message);
      g_variant_get_child (parameters, 1, "u", &percentage);
      add_status_line (tp, message, percentage);
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
      add_status_line (tp, line, -1);
    }
  else if (g_strcmp0 (signal_name, "ProgressEnd") == 0)
    {
      end_status_line (tp);
    }
  else if (g_strcmp0 (signal_name, "Finished") == 0)
    {
      if (tp->error == NULL)
        {
          g_autofree char *error_message = NULL;
          gboolean success = FALSE;

          g_variant_get (parameters, "(bs)", &success, &error_message);

          if (!success)
            {
              tp->error = g_dbus_error_new_for_dbus_error ("org.projectatomic.rpmostreed.Error.Failed",
                                                           error_message);
            }
        }

      transaction_progress_end (tp);
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
cancelled_handler (GCancellable *cancellable,
                   gpointer user_data)
{
  RPMOSTreeTransaction *transaction = user_data;
  rpmostree_transaction_call_cancel_sync (transaction, NULL, NULL);
}

static gboolean
on_sigint (gpointer user_data)
{
  GCancellable *cancellable = user_data;
  g_printerr ("Caught SIGINT, cancelling transaction\n");
  g_cancellable_cancel (cancellable);
  return TRUE;
}

gboolean
rpmostree_transaction_get_response_sync (RPMOSTreeSysroot *sysroot_proxy,
                                         const char *transaction_address,
                                         GCancellable *cancellable,
                                         GError **error)
{
  guint sigintid = 0;
  GDBusConnection *connection;
  glnx_unref_object GDBusObjectManager *object_manager = NULL;
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  g_autoptr(GDBusConnection) peer_connection = NULL;

  TransactionProgress *tp = transaction_progress_new ();

  const char *bus_name = NULL;
  gint cancel_handler;
  gulong signal_handler = 0;
  gboolean success = FALSE;
  gboolean just_started = FALSE;

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

  peer_connection = g_dbus_connection_new_for_address_sync (transaction_address,
                                                            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                            NULL,
                                                            cancellable,
                                                            error);

  if (peer_connection == NULL)
    goto out;

  transaction = rpmostree_transaction_proxy_new_sync (peer_connection,
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      NULL,
                                                      "/",
                                                      cancellable,
                                                      error);
  if (transaction == NULL)
    goto out;

  sigintid = g_unix_signal_add (SIGINT, on_sigint, cancellable);

  /* setup cancel handler */
  cancel_handler = g_cancellable_connect (cancellable,
                                          G_CALLBACK (cancelled_handler),
                                          transaction, NULL);

  signal_handler = g_signal_connect (transaction, "g-signal",
                                     G_CALLBACK (on_transaction_progress),
                                     tp);

  /* Tell the server we're ready to receive signals. */
  if (!rpmostree_transaction_call_start_sync (transaction,
                                              &just_started,
                                              cancellable,
                                              error))
    goto out;

  /* FIXME Use the 'just_started' flag to determine whether to print
   *       a message about reattaching to an in-progress transaction,
   *       like:
   *
   *       Existing upgrade in progress, reattaching.  Control-C to cancel.
   *
   *       But that requires having a printable description of the
   *       operation.  Maybe just add a string arg to this function?
   */

  g_main_loop_run (tp->loop);

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
  if (sigintid)
    g_source_remove (sigintid);
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
      if (i != 0)
        g_string_append_c (sigs_buffer, '\n');
      g_variant_get_child (variant, i, "v", &v);
      ostree_gpg_verify_result_describe_variant (v, sigs_buffer, sep,
                                                 OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
    }

  g_print ("%s", sigs_buffer->str);
  g_string_free (sigs_buffer, TRUE);
}

static gint
pkg_diff_variant_compare (gconstpointer a,
                          gconstpointer b,
                          gpointer unused)
{
  const char *pkg_name_a = NULL;
  const char *pkg_name_b = NULL;

  g_variant_get_child ((GVariant *) a, 0, "&s", &pkg_name_a);
  g_variant_get_child ((GVariant *) b, 0, "&s", &pkg_name_b);

  /* XXX Names should be unique since we're comparing packages
   *     from two different trees... right? */

  return g_strcmp0 (pkg_name_a, pkg_name_b);
}

static void
pkg_diff_variant_print (GVariant *variant)
{
  g_autoptr(GVariant) details = NULL;
  const char *old_name, *old_evr, *old_arch;
  const char *new_name, *new_evr, *new_arch;
  gboolean have_old = FALSE;
  gboolean have_new = FALSE;

  details = g_variant_get_child_value (variant, 2);
  g_return_if_fail (details != NULL);

  have_old = g_variant_lookup (details,
                               "PreviousPackage", "(&s&s&s)",
                               &old_name, &old_evr, &old_arch);

  have_new = g_variant_lookup (details,
                               "NewPackage", "(&s&s&s)",
                               &new_name, &new_evr, &new_arch);

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

  g_return_if_fail (variant != NULL);

  g_variant_iter_init (&iter, variant);

  /* Queue takes ownership of the child variant. */
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    g_queue_insert_sorted (&queue, child, pkg_diff_variant_compare, NULL);

  while (!g_queue_is_empty (&queue))
    {
      child = g_queue_pop_head (&queue);
      pkg_diff_variant_print (child);
      g_variant_unref (child);
    }
}

/* swiss-army knife: takes an strv of pkgspecs destined for
 * install, and splits it into repo pkgs, and for local
 * pkgs, an fd list & idx variant. */
gboolean
rpmostree_sort_pkgs_strv (const char *const* pkgs,
                          GPtrArray   **out_repo_pkgs,
                          GUnixFDList **out_fd_list,
                          GVariant    **out_fd_idxs,
                          GError      **error)
{
  g_autoptr(GPtrArray) repo_pkgs = g_ptr_array_new_with_free_func (g_free);
  glnx_unref_object GUnixFDList *fd_list = g_unix_fd_list_new ();
  g_auto(GVariantBuilder) builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("ah"));
  for (const char *const* pkg = pkgs; pkg && *pkg; pkg++)
    {
      if (!g_str_has_suffix (*pkg, ".rpm"))
        g_ptr_array_add (repo_pkgs, g_strdup (*pkg));
      else
        {
          glnx_fd_close int fd = open (*pkg, O_RDONLY | O_CLOEXEC);
          if (fd < 0)
            return glnx_throw_errno_prefix (error, "can't open '%s'", *pkg);

          int idx = g_unix_fd_list_append (fd_list, fd, error);
          if (idx < 0)
            return FALSE;

          g_variant_builder_add (&builder, "h", idx);
        }
    }

  *out_fd_list = g_steal_pointer (&fd_list);
  *out_fd_idxs = g_variant_ref_sink (g_variant_new ("ah", &builder));
  *out_repo_pkgs = g_steal_pointer (&repo_pkgs);
  return TRUE;
}
