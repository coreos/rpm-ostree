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

#include "daemon.h"
#include "types.h"
#include "manager.h"
#include "ostree.h"

/**
 * SECTION:daemon
 * @title: Daemon
 * @short_description: Main daemon object
 *
 * Object holding all global state.
 */

enum {
  FINISHED,
  NUM_SIGNALS
};

static guint64 MAX_MESSAGE_WAIT_SECONDS = 30;

static guint signals[NUM_SIGNALS] = { 0, };

typedef struct _DaemonClass DaemonClass;

/**
 * Daemon:
 *
 * The #Daemon structure contains only private data and should only be
 * accessed using the provided API.
 */
struct _Daemon
{
  GObject parent_instance;

  gint name_owner_id;
  gboolean name_owned;
  gboolean persist;

  gint64 last_message;
  guint ticker_id;

  GMutex mutex;
  GHashTable *watched_clients;

  gint num_tasks;

  GDBusConnection *connection;
  GDBusObjectManagerServer *object_manager;
  RPMOSTreeManager *manager;
};


struct _DaemonClass
{
  GObjectClass parent_class;
};

static Daemon *_daemon_instance;

enum
{
  PROP_0,
  PROP_CONNECTION,
  PROP_OBJECT_MANAGER,
  PROP_PERSIST,
};

G_DEFINE_TYPE (Daemon, daemon, G_TYPE_OBJECT);

typedef struct {
  GAsyncReadyCallback callback;
  gpointer callback_data;
  gpointer proxy_source;
} DaemonTaskData;


static void
daemon_finalize (GObject *object)
{
  Daemon *self = DAEMON (object);
  if (self->name_owner_id)
    g_bus_unown_name (self->name_owner_id);

  g_object_unref (self->object_manager);
  self->object_manager = NULL;

  g_object_unref (self->manager);
  g_object_unref (self->connection);

  if (self->ticker_id > 0)
    g_source_remove (self->ticker_id);

  g_mutex_lock (&self->mutex);
  g_hash_table_remove_all (self->watched_clients);
  g_hash_table_unref (self->watched_clients);
  g_mutex_unlock (&self->mutex);

  g_mutex_clear (&self->mutex);

  G_OBJECT_CLASS (daemon_parent_class)->finalize (object);

  _daemon_instance = NULL;
}


static void
daemon_get_property (GObject *object,
                     guint prop_id,
                     GValue *value,
                     GParamSpec *pspec)
{
  Daemon *self = DAEMON (object);

  switch (prop_id)
    {
    case PROP_OBJECT_MANAGER:
      g_value_set_object (value, self->object_manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
daemon_set_property (GObject *object,
                     guint prop_id,
                     const GValue *value,
                     GParamSpec *pspec)
{
  Daemon *self = DAEMON (object);

  switch (prop_id)
    {
    case PROP_CONNECTION:
      g_assert (self->connection == NULL);
      self->connection = g_value_dup_object (value);
      break;
   case PROP_PERSIST:
      self->persist = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
daemon_init (Daemon *self)
{
  g_assert (_daemon_instance == NULL);
  _daemon_instance = self;

  self->num_tasks = 0;
  self->last_message = g_get_monotonic_time ();

  self->watched_clients = g_hash_table_new_full (g_str_hash,
                                                 g_str_equal,
                                                 g_free,
                                                 NULL);
  g_mutex_init (&self->mutex);
}


static void
maybe_finished (Daemon *self)
{
  gboolean done = FALSE;

  if (self->name_owned == FALSE)
    {
      done = TRUE;
      g_debug ("We don't own the name, quit");
    }
  else if (g_atomic_int_get(&self->num_tasks) != 0)
    {
      g_debug ("Tasks running, stay alive");
    }
  else
    {
      // if we can't get the lock right away, stay alive.
      if (g_mutex_trylock (&self->mutex))
        {
          guint64 oldest_allowed = g_get_monotonic_time () - (MAX_MESSAGE_WAIT_SECONDS * G_USEC_PER_SEC);
          if (g_hash_table_size (self->watched_clients) == 0 &&
              oldest_allowed > self->last_message)
            {
              g_debug ("Too long since last message, closing");
              done = TRUE;
            }
          g_mutex_unlock (&self->mutex);
        }
    }

  if (done && !self->persist)
    g_signal_emit (self, signals[FINISHED], 0);
}


static gboolean
on_tick (gpointer user_data)
{
  Daemon *daemon = DAEMON (user_data);

  maybe_finished (daemon);
  return TRUE;
}


static void
on_client_lost (GDBusConnection *connection,
                const gchar *bus_name,
                gpointer user_data)
{
  Daemon *self = DAEMON (user_data);
  gpointer value = NULL;

  g_mutex_lock (&self->mutex);
  value = g_hash_table_lookup (self->watched_clients, bus_name);
  if (value != NULL)
    {
      guint watched = GPOINTER_TO_UINT (value);
      g_debug ("Losing client %s %d", bus_name, watched);
      g_hash_table_remove (self->watched_clients, bus_name);
      g_bus_unwatch_name (watched);
    }
  g_mutex_unlock (&self->mutex);
}

/**
 * daemon_watch_client:
 *
 * Tells the daemon to stay alive until the
 * #bus_name is released on the dbus connection.
 *
 * Returns: A new GTask*. Free with g_object_unref ()
 */
void
daemon_watch_client (Daemon *self,
                     GDBusConnection *connection,
                     const gchar *bus_name)
{
  g_mutex_lock (&self->mutex);
  if (!g_hash_table_contains (self->watched_clients,
                              bus_name))
    {
      guint watched = g_bus_watch_name_on_connection (connection, bus_name,
                                                G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                NULL, on_client_lost, self, NULL);
      g_debug ("Watching client %s %d", bus_name, watched);
      g_hash_table_insert (self->watched_clients,
                           g_strdup (bus_name),
                           GUINT_TO_POINTER (watched));
    }
  g_mutex_unlock (&self->mutex);
}


static void
on_daemon_task_completed (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  Daemon *self;
  DaemonTaskData *task_data = user_data;


  g_debug ("Daemon task callback");
  // Call the chained callback
  task_data->callback (source_object, res, task_data->callback_data);
  g_debug ("task done");
  g_return_if_fail (IS_DAEMON (task_data->proxy_source));

  self = DAEMON (task_data->proxy_source);

  // decrement counter and set last message to now
  g_assert (self->num_tasks > 0);
  g_atomic_int_dec_and_test (&self->num_tasks);

  g_mutex_lock (&self->mutex);
  self->last_message = g_get_monotonic_time ();
  g_mutex_unlock (&self->mutex);

  // release refs we took in daemon_get_new_task
  g_object_unref (source_object);
  g_object_unref (self);

  g_slice_free (DaemonTaskData, task_data);
}


/**
 * daemon_get_new_task:
 *
 * Creates a new task that is tracked by the daemon.
 * Daemon will stay alive until task is done.
 *
 * Returns: A new GTask*. Free with g_object_unref ()
 * ei: task is not be freed in daemon callback
 */
GTask *
daemon_get_new_task (Daemon *self,
                     gpointer source_object,
                     GCancellable *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer callback_data)
{
  DaemonTaskData *task_data = NULL;

  task_data = g_slice_new (DaemonTaskData);
  task_data->callback = callback;
  task_data->callback_data = callback_data;
  task_data->proxy_source = g_object_ref (self);

  g_object_ref (source_object);

  g_atomic_int_inc (&self->num_tasks);
  return g_task_new (source_object, cancellable,
                     on_daemon_task_completed,
                     task_data);
}

static GDBusMessage *
on_connection_filter (GDBusConnection *connection,
                      GDBusMessage *message,
                      gboolean incoming,
                      gpointer user_data)
{
  Daemon *daemon = DAEMON (user_data);
  // Record time of last incoming message

  if (incoming)
    {
      g_mutex_lock (&daemon->mutex);
      daemon->last_message = g_get_monotonic_time ();
      g_mutex_unlock (&daemon->mutex);
    }

  return message;
}


static void
on_name_lost (GDBusConnection *connection,
              const gchar *name,
              gpointer user_data)
{
  Daemon *self = user_data;
  g_message ("Lost (or failed to acquire) the name %s on the system message bus", name);
  self->name_owned = FALSE;
  maybe_finished (self);
}


static void
on_name_acquired (GDBusConnection *connection,
                  const gchar *bus_name,
                  gpointer user_data)
{
  Daemon *self = user_data;
  g_info ("Acquired the name %s on the system message bus", bus_name);
  self->name_owned = TRUE;
}


static void
daemon_constructed (GObject *_object)
{
  Daemon *self;

  self = DAEMON (_object);
  self->object_manager = g_dbus_object_manager_server_new ("/org/projectatomic/rpmostree1");
  /* Export the ObjectManager */
  g_dbus_object_manager_server_set_connection (self->object_manager, self->connection);
  g_debug ("exported object manager");

  /* /org/projectatomic/rpmostree1/Manager */
  self->manager = g_object_new (RPM_OSTREE_TYPE_DAEMON_MANAGER,
                                NULL);
  daemon_publish (self, "/org/projectatomic/rpmostree1/Manager", FALSE, self->manager);

  self->name_owner_id = g_bus_own_name_on_connection (self->connection,
                                                      "org.projectatomic.rpmostree1",
                                                      G_BUS_NAME_OWNER_FLAGS_NONE,
                                                      on_name_acquired,
                                                      on_name_lost,
                                                      self,
                                                      NULL);

  // Setup message checks
  self->ticker_id = g_timeout_add_seconds (MAX_MESSAGE_WAIT_SECONDS,
                                           on_tick, self);
  g_dbus_connection_add_filter (self->connection,
                                on_connection_filter,
                                self, NULL);

  G_OBJECT_CLASS (daemon_parent_class)->constructed (_object);

  g_debug ("daemon constructed");
}

static void
daemon_class_init (DaemonClass *klass)
{
  GObjectClass *gobject_class;
  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = daemon_finalize;
  gobject_class->constructed  = daemon_constructed;
  gobject_class->set_property = daemon_set_property;
  gobject_class->get_property = daemon_get_property;

  /**
   * Daemon:connection:
   *
   * The #GDBusConnection the daemon is for.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_CONNECTION,
                                   g_param_spec_object ("connection",
                                                        "Connection",
                                                        "The D-Bus connection the daemon is for",
                                                        G_TYPE_DBUS_CONNECTION,
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * Daemon:object-manager:
   *
   * The #GDBusObjectManager used by the daemon
   */
  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_MANAGER,
                                   g_param_spec_object ("object-manager",
                                                        "Object Manager",
                                                        "The D-Bus Object Manager server used by the daemon",
                                                        G_TYPE_DBUS_OBJECT_MANAGER_SERVER,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PERSIST,
                                   g_param_spec_boolean ("persist",
                                                         "Persist",
                                                         "Don't stop daemon automatically",
                                                         FALSE,
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS));

  signals[FINISHED] = g_signal_new ("finished",
                                    TYPE_DAEMON,
                                    G_SIGNAL_RUN_LAST,
                                    0, NULL, NULL,
                                    g_cclosure_marshal_generic,
                                    G_TYPE_NONE, 0);
}

/**
 * daemon_get:
 *
 * Returns: (transfer none): The singleton #Daemon instance
 */
Daemon *
daemon_get (void)
{
  g_assert (_daemon_instance);
  return _daemon_instance;
}


void
daemon_publish (Daemon *self,
                const gchar *path,
                gboolean uniquely,
                gpointer thing)
{
  GDBusInterface *prev = NULL;
  GDBusInterfaceInfo *info = NULL;
  GDBusObjectSkeleton *object = NULL;

  g_return_if_fail (IS_DAEMON (self));
  g_return_if_fail (path != NULL);

  if (G_IS_DBUS_INTERFACE (thing))
    {
      g_debug ("%spublishing iface: %s %s", uniquely ? "uniquely " : "", path,
               g_dbus_interface_get_info (thing)->name);

      object = G_DBUS_OBJECT_SKELETON (g_dbus_object_manager_get_object (G_DBUS_OBJECT_MANAGER (self->object_manager), path));
      if (object != NULL)
        {
          if (uniquely)
            {
              info = g_dbus_interface_get_info (thing);
              prev = g_dbus_object_get_interface (G_DBUS_OBJECT (object), info->name);
              if (prev)
                {
                  g_object_unref (prev);
                  g_object_unref (object);
                  object = NULL;
                }
            }
        }

      if (object == NULL)
        object = g_dbus_object_skeleton_new (path);

      g_dbus_object_skeleton_add_interface (object, thing);
    }
  else
    {
      g_critical ("Unsupported type to publish: %s", G_OBJECT_TYPE_NAME (thing));
      return;
    }

  if (uniquely)
    g_dbus_object_manager_server_export_uniquely (self->object_manager, object);
  else
    g_dbus_object_manager_server_export (self->object_manager, object);

  if (object)
    g_object_unref (object);
}

GDBusInterface *
daemon_get_interface (Daemon *self,
                      const gchar *object_path,
                      const gchar *interface_name)
{
  return g_dbus_object_manager_get_interface (G_DBUS_OBJECT_MANAGER (self->object_manager),
                                              object_path, interface_name);
}

void
daemon_unpublish (Daemon *self,
                  const gchar *path,
                  gpointer thing)
{
  GDBusObject *object;
  gboolean unexport = FALSE;
  GList *interfaces, *l;

  g_return_if_fail (IS_DAEMON (self));
  g_return_if_fail (path != NULL);

  if (self->object_manager == NULL)
    return;

  object = g_dbus_object_manager_get_object (G_DBUS_OBJECT_MANAGER (self->object_manager), path);
  if (object == NULL)
    return;

  path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));
  if (G_IS_DBUS_INTERFACE (thing))
    {
      g_debug ("unpublishing interface: %s %s", path,
               g_dbus_interface_get_info (thing)->name);

      unexport = TRUE;

      interfaces = g_dbus_object_get_interfaces (object);
      for (l = interfaces; l != NULL; l = g_list_next (l))
        {
          if (G_DBUS_INTERFACE (l->data) != G_DBUS_INTERFACE (thing))
            unexport = FALSE;
        }
      g_list_free_full (interfaces, g_object_unref);

      /*
      * HACK: GDBusObjectManagerServer is broken ... and sends InterfaceRemoved
      * too many times, if you remove all interfaces manually, and then unexport
      * a GDBusObject. So only do it here if we're not unexporting the object.
      */
      if (!unexport)
        g_dbus_object_skeleton_remove_interface (G_DBUS_OBJECT_SKELETON (object), thing);
      else
        g_debug ("(unpublishing object, too)");
    }
  else if (thing == NULL)
    {
      unexport = TRUE;
    }
  else
    {
      g_critical ("Unsupported type to unpublish: %s", G_OBJECT_TYPE_NAME (thing));
    }

  if (unexport)
    g_dbus_object_manager_server_unexport (self->object_manager, path);

  g_object_unref (object);
}
