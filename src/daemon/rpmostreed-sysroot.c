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

#include "rpmostreed-daemon.h"
#include "rpmostreed-sysroot.h"
#include "rpmostreed-os.h"
#include "rpmostreed-utils.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-errors.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-transaction-monitor.h"

#include "libgsystem.h"
#include <libglnx.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

/**
 * SECTION: sysroot
 * @title: RpmostreedSysroot
 * @short_description: Implementation of #RPMOSTreeSysroot
 *
 * This type provides an implementation of the #RPMOSTreeSysroot interface.
 */

typedef struct _RpmostreedSysrootClass RpmostreedSysrootClass;

/**
 * RpmostreedSysroot:
 *
 * The #RpmostreedSysroot structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _RpmostreedSysroot {
  RPMOSTreeSysrootSkeleton parent_instance;

  GCancellable *cancellable;

  OstreeSysroot *ot_sysroot;
  RpmostreedTransactionMonitor *transaction_monitor;

  GHashTable *os_interfaces;
  GRWLock children_lock;

  /* The OS interface's various diff methods can run concurrently with
   * transactions, which is safe except when the transaction is writing
   * new deployments to disk or downloading RPM package details.  The
   * writer lock protects these critical sections so the diff methods
   * (holding reader locks) can run safely. */
  GRWLock method_rw_lock;

  GFileMonitor *monitor;
  guint sig_changed;
  guint64 last_monitor_event;

  guint stdout_source_id;
};

struct _RpmostreedSysrootClass {
  RPMOSTreeSysrootSkeletonClass parent_class;

  void          (*updated)              (RpmostreedSysroot *self,
                                         OstreeSysroot *ot_sysroot,
                                         OstreeRepo *ot_repo);
};

enum {
  UPDATED,
  NUM_SIGNALS
};

static guint64 UPDATED_THROTTLE_SECONDS = 2;
static guint signals[NUM_SIGNALS];

static void rpmostreed_sysroot_iface_init (RPMOSTreeSysrootIface *iface);
static gboolean _throttle_refresh (gpointer user_data);

G_DEFINE_TYPE_WITH_CODE (RpmostreedSysroot,
                         rpmostreed_sysroot,
                         RPMOSTREE_TYPE_SYSROOT_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_SYSROOT,
                                                rpmostreed_sysroot_iface_init));

static RpmostreedSysroot *_sysroot_instance;

/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
  GWeakRef sysroot;
  GOutputStream *real_stdout;
  GDataInputStream *data_stream;
} StdoutClosure;

static void
stdout_closure_free (StdoutClosure *closure)
{
  g_weak_ref_set (&closure->sysroot, NULL);
  g_clear_object (&closure->real_stdout);
  g_clear_object (&closure->data_stream);
  g_slice_free (StdoutClosure, closure);
}

static gboolean
sysroot_stdout_ready_cb (GPollableInputStream *pollable_stream,
                         StdoutClosure *closure)
{
  glnx_unref_object RpmostreedSysroot *sysroot = NULL;
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  GMemoryInputStream *memory_stream;
  GBufferedInputStream *buffered_stream;
  char buffer[1024];
  gconstpointer stream_buffer;
  gsize stream_buffer_size;
  gsize total_bytes_read = 0;
  gboolean have_line = FALSE;
  GError *local_error = NULL;

  sysroot = g_weak_ref_get (&closure->sysroot);
  if (sysroot != NULL)
    transaction = rpmostreed_transaction_monitor_ref_active_transaction (sysroot->transaction_monitor);

  /* XXX Would very much like g_buffered_input_stream_fill_nonblocking().
   *     Much of this function is a clumsy and inefficient attempt to
   *     achieve the same thing.
   *
   *     See: https://bugzilla.gnome.org/726797
   */

  buffered_stream = G_BUFFERED_INPUT_STREAM (closure->data_stream);
  memory_stream = (GMemoryInputStream *) g_filter_input_stream_get_base_stream (G_FILTER_INPUT_STREAM (buffered_stream));

  while (local_error == NULL)
    {
      gssize n_read;

      n_read = g_pollable_input_stream_read_nonblocking (pollable_stream,
                                                         buffer,
                                                         sizeof (buffer),
                                                         NULL, &local_error);

      if (n_read > 0)
        {
          /* XXX Gotta use GBytes so the data gets copied. */
          g_autoptr(GBytes) bytes = g_bytes_new (buffer, n_read);
          g_memory_input_stream_add_bytes (memory_stream, bytes);
          total_bytes_read += n_read;
        }
    }

  if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
    goto out;

  g_clear_error (&local_error);

read_another_line:

  /* Fill the buffered stream with the data we just put in the
   * memory stream, then peek at the buffer to see if it's safe
   * to call g_data_input_stream_read_line() without blocking.
   *
   * XXX Oye, there's gotta be an easier way to do this...
   */

  /* This should never fail since it's just reading from memory. */
  g_buffered_input_stream_fill (buffered_stream, total_bytes_read, NULL, NULL);

  stream_buffer = g_buffered_input_stream_peek_buffer (buffered_stream, &stream_buffer_size);
  have_line = (memchr (stream_buffer, '\n', stream_buffer_size) != NULL);

  if (have_line)
    {
      g_autofree char *line = NULL;
      gsize length;

      line = g_data_input_stream_read_line (closure->data_stream,
                                            &length, NULL, &local_error);

      if (local_error != NULL)
        goto out;

      /* If there's an active transaction, forward the line to the
       * transaction's owner through the "Message" signal.  Otherwise
       * dump it to the non-redirected standard output stream. */
      if (transaction != NULL)
        {
          rpmostree_transaction_emit_message (RPMOSTREE_TRANSACTION (transaction), line);
        }
      else
        {
          /* This is essentially puts(), don't care about errors. */
          g_output_stream_write_all (closure->real_stdout,
                                     line, length, NULL, NULL, NULL);
          g_output_stream_write_all (closure->real_stdout,
                                     "\n", 1, NULL, NULL, NULL);
        }

      goto read_another_line;
    }

out:
  if (local_error != NULL)
    {
      g_warning ("Failed to read stdout pipe: %s", local_error->message);
      g_clear_error (&local_error);
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
sysroot_setup_stdout_redirect (RpmostreedSysroot *self,
                               GError **error)
{
  g_autoptr(GInputStream) stream = NULL;
  g_autoptr(GSource) source = NULL;
  StdoutClosure *closure;
  gint pipefd[2];
  gboolean ret = FALSE;

  /* XXX libostree logs messages to systemd's journal and also to stdout.
   *     Redirect our own stdout back to ourselves so we can capture those
   *     messages and pass them on to clients.  Admittedly hokey but avoids
   *     hacking libostree directly (for now). */

  closure = g_slice_new0 (StdoutClosure);
  g_weak_ref_set (&closure->sysroot, self);

  /* Save the real stdout before overwriting its file descriptor. */
  closure->real_stdout = g_unix_output_stream_new (dup (STDOUT_FILENO), FALSE);

  if (pipe (pipefd) < 0)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "pipe() failed");
      goto out;
    }

  if (dup2 (pipefd[1], STDOUT_FILENO) < 0)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "dup2() failed");
      goto out;
    }

  stream = g_memory_input_stream_new ();
  closure->data_stream = g_data_input_stream_new (stream);
  g_clear_object (&stream);

  stream = g_unix_input_stream_new (pipefd[0], FALSE);

  source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (stream),
                                                  NULL);
  /* Transfer ownership of the StdoutClosure. */
  g_source_set_callback (source,
                         (GSourceFunc) sysroot_stdout_ready_cb,
                         closure,
                         (GDestroyNotify) stdout_closure_free);
  closure = NULL;

  self->stdout_source_id = g_source_attach (source, NULL);

  ret = TRUE;

out:
  if (closure != NULL)
    stdout_closure_free (closure);

  return ret;
}

static GFile *
_build_file (const char *first, ...)
{
  va_list args;
  const gchar *arg;
  g_autofree gchar *path = NULL;
  g_autoptr(GPtrArray) parts = NULL;

  va_start (args, first);
  parts = g_ptr_array_new ();
  arg = first;
  while (arg != NULL)
    {
      g_ptr_array_add (parts, (char*)arg);
      arg = va_arg (args, const char *);
    }
  va_end (args);

  g_ptr_array_add (parts, NULL);
  path = g_build_filenamev ((gchar**)parts->pdata);
  return g_file_new_for_path (path);
}

static gboolean
handle_create_osname (RPMOSTreeSysroot *object,
                      GDBusMethodInvocation *invocation,
                      const gchar *osname)
{
  g_autoptr(GFile) dir = NULL;
  g_autofree gchar *deploy_dir = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  const char *sysroot_path;

  GError *error = NULL;
  g_autofree gchar *dbus_path = NULL;

  gboolean needs_refresh = FALSE;

  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);

  if (!rpmostreed_sysroot_load_state (self,
                                      self->cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &error))
    goto out;

  if (!ostree_sysroot_ensure_initialized (ot_sysroot,
                                          self->cancellable,
                                          &error))
    goto out;

  if (strchr (osname, '/') != 0)
    {
      g_set_error_literal (&error,
                           RPM_OSTREED_ERROR,
                           RPM_OSTREED_ERROR_FAILED,
                           "Invalid osname");
      goto out;
    }

  if (!ostree_sysroot_init_osname (ot_sysroot, osname, self->cancellable, error))
    goto out;

  dbus_path = rpmostreed_generate_object_path (BASE_DBUS_PATH, osname, NULL);
  g_rw_lock_reader_lock (&self->children_lock);
    needs_refresh = self->last_monitor_event == 0;
  g_rw_lock_reader_unlock (&self->children_lock);

  if (needs_refresh)
    _throttle_refresh (self);

  rpmostree_sysroot_complete_create_osname (RPMOSTREE_SYSROOT (self),
                                            invocation,
                                            g_strdup (dbus_path));
out:
  if (error)
    g_dbus_method_invocation_take_error (invocation, error);

  return TRUE;
}

static gboolean
handle_get_os (RPMOSTreeSysroot *object,
               GDBusMethodInvocation *invocation,
               const char *arg_name)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  glnx_unref_object GDBusInterfaceSkeleton *os_interface = NULL;

  if (arg_name[0] == '\0')
    {
      rpmostree_sysroot_complete_get_os (object,
                                         invocation,
                                         rpmostree_sysroot_dup_booted (object));
      goto out;
    }

  g_rw_lock_reader_lock (&self->children_lock);

  os_interface = g_hash_table_lookup (self->os_interfaces, arg_name);
  if (os_interface != NULL)
    g_object_ref (os_interface);

  g_rw_lock_reader_unlock (&self->children_lock);

  if (os_interface != NULL)
    {
      const char *object_path;
      object_path = g_dbus_interface_skeleton_get_object_path (os_interface);
      rpmostree_sysroot_complete_get_os (object, invocation, object_path);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_FOUND,
                                             "OS name \"%s\" not found",
                                             arg_name);
    }

out:
  return TRUE;
}

static gboolean
sysroot_transform_transaction_to_attrs (GBinding *binding,
                                        const GValue *src_value,
                                        GValue *dst_value,
                                        gpointer user_data)
{
  RpmostreedTransaction *transaction;
  GVariant *variant;
  const char *method_name = "";
  const char *path = "";
  const char *sender_name = "";

  transaction = g_value_get_object (src_value);

  if (transaction != NULL)
    {
      GDBusMethodInvocation *invocation;

      invocation = rpmostreed_transaction_get_invocation (transaction);
      method_name = g_dbus_method_invocation_get_method_name (invocation);
      path = g_dbus_method_invocation_get_object_path (invocation);
      sender_name = g_dbus_method_invocation_get_sender (invocation);
    }

  variant = g_variant_new ("(sss)", method_name, sender_name, path);

  g_value_set_variant (dst_value, variant);

  return TRUE;
}

static void
sysroot_populate_deployments (RpmostreedSysroot *self,
                              OstreeSysroot *ot_sysroot,
                              OstreeRepo *ot_repo)
{
  OstreeDeployment *booted = NULL; /* owned by sysroot */
  g_autoptr(GPtrArray) deployments = NULL;
  g_autoptr(GHashTable) seen_osnames = NULL;
  GHashTableIter iter;
  gpointer hashkey;
  gpointer value;
  GVariantBuilder builder;
  guint i;

  g_debug ("loading deployments");
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));

  seen_osnames = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);

  /* Add deployment interfaces */
  deployments = ostree_sysroot_get_deployments (ot_sysroot);

  for (i = 0; deployments != NULL && i < deployments->len; i++)
    {
      GVariant *variant;
      OstreeDeployment *deployment = deployments->pdata[i];
      const char *deployment_os;
      
      variant = rpmostreed_deployment_generate_variant (deployment, ot_repo);
      g_variant_builder_add_value (&builder, variant);

      deployment_os = ostree_deployment_get_osname (deployment);

      /* Have we not seen this osname instance before?  If so, add it
       * now.
       */
      if (!g_hash_table_contains (self->os_interfaces, deployment_os))
	{
	  RPMOSTreeOS *obj = rpmostreed_os_new (ot_sysroot, ot_repo,
						deployment_os,
                                                self->transaction_monitor);
          g_hash_table_insert (self->os_interfaces, g_strdup (deployment_os), obj);
	}
      /* Owned by deployment, hash lifetime is smaller */
      g_hash_table_add (seen_osnames, (char*)deployment_os);
    }

  booted = ostree_sysroot_get_booted_deployment (ot_sysroot);
  if (booted)
    {
      const gchar *os = ostree_deployment_get_osname (booted);
      g_autofree gchar *path = rpmostreed_generate_object_path (BASE_DBUS_PATH,
                                                                os, NULL);
      rpmostree_sysroot_set_booted (RPMOSTREE_SYSROOT (self), path);
    }
  else
    {
      rpmostree_sysroot_set_booted (RPMOSTREE_SYSROOT (self), "/");
    }

  /* Remove dead os paths */
  g_hash_table_iter_init (&iter, self->os_interfaces);
  while (g_hash_table_iter_next (&iter, &hashkey, &value))
    {
      if (!g_hash_table_contains (seen_osnames, hashkey))
        {
          g_object_run_dispose (G_OBJECT (value));
          g_hash_table_iter_remove (&iter);
        }
    }

  rpmostree_sysroot_set_deployments (RPMOSTREE_SYSROOT (self),
                                     g_variant_builder_end (&builder));
  g_debug ("finished deployments");
}

/* ---------------------------------------------------------------------------------------------------- */
static void
sysroot_dispose (GObject *object)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  GHashTableIter iter;
  gpointer value;
  gint tries;

  g_cancellable_cancel (self->cancellable);

  if (self->monitor)
    {
      if (self->sig_changed)
        g_signal_handler_disconnect (self->monitor, self->sig_changed);
      self->sig_changed = 0;

      /* HACK - It is not generally safe to just unref a GFileMonitor.
       * Some events might be on their way to the main loop from its
       * worker thread and if they arrive after the GFileMonitor has
       * been destroyed, bad things will happen.
       *
       * As a workaround, we cancel the monitor and then spin the main
       * loop a bit until nothing is pending anymore.
       *
       * https://bugzilla.gnome.org/show_bug.cgi?id=740491
       */

      g_file_monitor_cancel (self->monitor);
      for (tries = 0; tries < 10; tries ++)
        {
          if (!g_main_context_iteration (NULL, FALSE))
            break;
        }
    }

  g_rw_lock_writer_lock (&self->children_lock);

  /* Tracked os paths are responsible to unpublish themselves */
  g_hash_table_iter_init (&iter, self->os_interfaces);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    g_object_run_dispose (value);
  g_hash_table_remove_all (self->os_interfaces);

  g_rw_lock_writer_unlock (&self->children_lock);

  g_clear_object (&self->transaction_monitor);

  G_OBJECT_CLASS (rpmostreed_sysroot_parent_class)->dispose (object);
}

static void
sysroot_finalize (GObject *object)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  _sysroot_instance = NULL;

  g_hash_table_unref (self->os_interfaces);

  g_rw_lock_clear (&self->children_lock);
  g_rw_lock_clear (&self->method_rw_lock);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->monitor);

  if (self->stdout_source_id > 0)
    g_source_remove (self->stdout_source_id);

  G_OBJECT_CLASS (rpmostreed_sysroot_parent_class)->finalize (object);
}

static void
rpmostreed_sysroot_init (RpmostreedSysroot *self)
{
  g_assert (_sysroot_instance == NULL);
  _sysroot_instance = self;

  self->cancellable = g_cancellable_new ();

  self->os_interfaces = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
  (GDestroyNotify) g_object_unref);

  g_rw_lock_init (&self->children_lock);
  g_rw_lock_init (&self->method_rw_lock);

  self->monitor = NULL;
  self->sig_changed = 0;
  self->last_monitor_event = 0;

  self->transaction_monitor = rpmostreed_transaction_monitor_new ();
}

static void
sysroot_constructed (GObject *object)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  GError *local_error = NULL;

  /* TODO Integrate with PolicyKit via the "g-authorize-method" signal. */

  g_object_bind_property_full (self->transaction_monitor,
                               "active-transaction",
                               self,
                               "active-transaction",
                               G_BINDING_DEFAULT |
                               G_BINDING_SYNC_CREATE,
                               sysroot_transform_transaction_to_attrs,
                               NULL,
                               NULL,
                               NULL);

  /* Failure is not fatal, but the client may miss some messages. */
  if (!sysroot_setup_stdout_redirect (self, &local_error))
    {
      g_critical ("%s", local_error->message);
      g_clear_error (&local_error);
    }

  G_OBJECT_CLASS (rpmostreed_sysroot_parent_class)->constructed (object);
}

static void
sysroot_updated (RpmostreedSysroot *self,
                 OstreeSysroot *ot_sysroot,
                 OstreeRepo *ot_repo)
{
  sysroot_populate_deployments (self, ot_sysroot, ot_repo);
}

static void
rpmostreed_sysroot_class_init (RpmostreedSysrootClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = sysroot_dispose;
  gobject_class->finalize     = sysroot_finalize;
  gobject_class->constructed  = sysroot_constructed;

  klass->updated = sysroot_updated;

  signals[UPDATED] = g_signal_new ("updated",
                                   RPMOSTREED_TYPE_SYSROOT,
                                   G_SIGNAL_RUN_LAST,
                                   G_STRUCT_OFFSET (RpmostreedSysrootClass, updated),
                                   NULL, NULL, NULL,
                                   G_TYPE_NONE, 2,
                                   OSTREE_TYPE_SYSROOT,
                                   OSTREE_TYPE_REPO);
}

static gboolean
rpmostreed_sysroot_load_internals (RpmostreedSysroot *self,
                                   OstreeSysroot *ot_sysroot,
                                   OstreeRepo *ot_repo,
                                   GError **error)
{
  g_rw_lock_writer_lock (&self->children_lock);
  sysroot_populate_deployments (self, ot_sysroot, ot_repo);
  g_rw_lock_writer_unlock (&self->children_lock);
  return TRUE;
}

static void
_do_reload_data (GTask         *task,
                 gpointer       object,
                 gpointer       data_ptr,
                 GCancellable  *cancellable)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  glnx_unref_object OstreeRepo *ot_repo = NULL;
  GError *local_error = NULL;

  if (!rpmostreed_sysroot_load_state (self,
                                      cancellable,
                                      &ot_sysroot,
                                      &ot_repo,
                                      &local_error))
    goto out;

  if (!rpmostreed_sysroot_load_internals (self,
                                          ot_sysroot,
                                          ot_repo,
                                          &local_error))
    goto out;

out:
  if (local_error != NULL)
    g_task_return_error (task, local_error);
  else
    g_task_return_boolean (task, TRUE);
}


static void
_reload_callback (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
    RpmostreedSysroot *self = RPMOSTREED_SYSROOT (user_data);
    GTask *task = G_TASK (res);
    GError *error = NULL;

    g_task_propagate_boolean (task, &error);
    if (error)
      {
        /* this was valid once, make sure it is tried again
         * TODO: should we bail at some point? */
        g_message ("Error refreshing sysroot data: %s", error->message);
        g_timeout_add_seconds (UPDATED_THROTTLE_SECONDS,
                               _throttle_refresh,
                               self);
      }
    else
      {
        rpmostreed_sysroot_emit_update (self);
      }

    g_object_unref (task);
    g_clear_error (&error);
}


static gboolean
_throttle_refresh (gpointer user_data)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (user_data);
  gboolean ret = TRUE;

  /* Only run the update if there isn't another one pending. */
  g_rw_lock_writer_lock (&self->children_lock);
  if ((g_get_monotonic_time () - self->last_monitor_event) >
      UPDATED_THROTTLE_SECONDS * G_USEC_PER_SEC)
    {
      GTask *task = g_task_new (self, self->cancellable,
                                _reload_callback, self);
      g_task_set_return_on_cancel (task, TRUE);
      self->last_monitor_event = 0;
      g_task_run_in_thread (task, _do_reload_data);
      ret = FALSE;
    }
  g_rw_lock_writer_unlock (&self->children_lock);

  return ret;
}

static void
on_repo_file (GFileMonitor *monitor,
              GFile *file,
              GFile *other_file,
              GFileMonitorEvent event_type,
              gpointer user_data)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (user_data);
  if (event_type == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
    {
      g_rw_lock_writer_lock (&self->children_lock);
      if (self->last_monitor_event == 0)
        g_timeout_add_seconds (UPDATED_THROTTLE_SECONDS,
                               _throttle_refresh,
                               user_data);

      self->last_monitor_event = g_get_monotonic_time ();
      g_rw_lock_writer_unlock (&self->children_lock);
    }
}



static void
rpmostreed_sysroot_iface_init (RPMOSTreeSysrootIface *iface)
{
  iface->handle_create_osname = handle_create_osname;
  iface->handle_get_os = handle_get_os;
}


/**
 * rpmostreed_sysroot_emit_update:
 *
 * Emits an updated signal
 */
void
rpmostreed_sysroot_emit_update (RpmostreedSysroot *self)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  glnx_unref_object OstreeRepo *ot_repo = NULL;
  GError *local_error = NULL;

  g_return_if_fail (RPMOSTREED_IS_SYSROOT (self));

  /* XXX Creating an OstreeSysroot and OstreeRepo instance are each
   *     failable, and this is a spot where creating and disposing of
   *     them repeatedly introduces some inconvenient error handling.
   *     Keeping persistent instances in RpmostreedSysroot would be
   *     preferable but there's issues around keeping their internal
   *     state up-to-date when ostree commands operate on their own. */

  if (rpmostreed_sysroot_load_state (self,
                                     NULL,
                                     &ot_sysroot,
                                     &ot_repo,
                                     &local_error))
    {
      g_signal_emit (self, signals[UPDATED], 0, ot_sysroot, ot_repo);
    }
  else
    {
      g_return_if_fail (local_error != NULL);
      g_critical ("Unable to update state: %s", local_error->message);
      g_clear_error (&local_error);
    }
}

/**
 * rpmostreed_sysroot_populate :
 *
 * loads internals and starts monitoring
 *
 * Returns: True on success
 */
gboolean
rpmostreed_sysroot_populate (RpmostreedSysroot *self,
                             GError **error)
{
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  glnx_unref_object OstreeRepo *ot_repo = NULL;
  gboolean ret = FALSE;

  g_return_val_if_fail (self != NULL, FALSE);

  if (!rpmostreed_sysroot_load_state (self,
                                      NULL,
                                      &ot_sysroot,
                                      &ot_repo,
                                      error))
    goto out;

  if (!rpmostreed_sysroot_load_internals (self, ot_sysroot, ot_repo, error))
    goto out;

  if (self->monitor == NULL)
    {
      GFile *repo_file = ostree_repo_get_path (ot_repo); /* owned by ot_repo */
      self->monitor = g_file_monitor (repo_file, 0, NULL, error);

      if (self->monitor == NULL)
        goto out;

      self->sig_changed = g_signal_connect (self->monitor,
                                            "changed",
                                            G_CALLBACK (on_repo_file),
                                            self);
    }

  ret = TRUE;

out:
  return ret;
}

gboolean
rpmostreed_sysroot_load_state (RpmostreedSysroot *self,
                               GCancellable *cancellable,
                               OstreeSysroot **out_sysroot,
                               OstreeRepo **out_repo,
                               GError **error)
{
  g_autoptr(GFile) sysroot_file = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *sysroot_path;
  gboolean ret = FALSE;

  g_return_val_if_fail (RPMOSTREED_IS_SYSROOT (self), FALSE);

  sysroot_path = rpmostree_sysroot_get_path (RPMOSTREE_SYSROOT (self));
  g_return_val_if_fail (sysroot_path != NULL, FALSE);

  sysroot_file = g_file_new_for_path (sysroot_path);

  sysroot = ostree_sysroot_new (sysroot_file);

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  /* This creates and caches an OstreeRepo instance inside OstreeSysroot.
   * Worth doing here even if the caller doesn't want the repo reference
   * to ensure subsequent ostree_sysroot_get_repo() calls won't fail. */
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  if (out_sysroot != NULL)
    *out_sysroot = g_steal_pointer (&sysroot);

  if (out_repo != NULL)
    *out_repo = g_steal_pointer (&repo);

  ret = TRUE;

out:
  return ret;
}

/**
 * rpmostreed_sysroot_get:
 *
 * Returns: (transfer none): The singleton #RpmostreedSysroot instance
 */
RpmostreedSysroot *
rpmostreed_sysroot_get (void)
{
  g_assert (_sysroot_instance);
  return _sysroot_instance;
}

void
rpmostreed_sysroot_reader_lock (RpmostreedSysroot *self)
{
  g_return_if_fail (RPMOSTREED_IS_SYSROOT (self));

  g_rw_lock_reader_lock (&self->method_rw_lock);
}

void
rpmostreed_sysroot_reader_unlock (RpmostreedSysroot *self)
{
  g_return_if_fail (RPMOSTREED_IS_SYSROOT (self));

  g_rw_lock_reader_unlock (&self->method_rw_lock);
}

void
rpmostreed_sysroot_writer_lock (RpmostreedSysroot *self)
{
  g_return_if_fail (RPMOSTREED_IS_SYSROOT (self));

  g_rw_lock_writer_lock (&self->method_rw_lock);
}

void
rpmostreed_sysroot_writer_unlock (RpmostreedSysroot *self)
{
  g_return_if_fail (RPMOSTREED_IS_SYSROOT (self));

  g_rw_lock_writer_unlock (&self->method_rw_lock);
}

/* ---------------------------------------------------------------------------------------------------- */
