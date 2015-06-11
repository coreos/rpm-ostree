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
#include "sysroot.h"
#include "types.h"
#include "utils.h"

#include <libglnx.h>

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

  gboolean on_message_bus;
  guint use_count;

  gint64 last_message;
  guint ticker_id;

  GMutex mutex;
  gint num_tasks;

  Sysroot *sysroot;
  gchar *sysroot_path;

  GDBusConnection *connection;
  GDBusObjectManagerServer *object_manager;
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
  PROP_SYSROOT_PATH,
  PROP_ON_MESSAGE_BUS,
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

  g_clear_object (&self->object_manager);
  self->object_manager = NULL;

  g_clear_object (&self->sysroot);

  g_object_unref (self->connection);

  if (self->ticker_id > 0)
    g_source_remove (self->ticker_id);

  g_mutex_clear (&self->mutex);

  g_free (self->sysroot_path);
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
    case PROP_SYSROOT_PATH:
      g_assert (self->sysroot_path == NULL);
      self->sysroot_path = g_value_dup_string (value);
      break;
   case PROP_ON_MESSAGE_BUS:
      self->on_message_bus = g_value_get_boolean (value);
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
  self->sysroot_path = NULL;
  self->sysroot = NULL;

  g_mutex_init (&self->mutex);
}

static void
daemon_constructed (GObject *_object)
{
  Daemon *self;
  GError *error = NULL;
  g_autofree gchar *path = NULL;

  self = DAEMON (_object);
  self->object_manager = g_dbus_object_manager_server_new (BASE_DBUS_PATH);
  /* Export the ObjectManager */
  g_dbus_object_manager_server_set_connection (self->object_manager, self->connection);
  g_debug ("exported object manager");

  G_OBJECT_CLASS (daemon_parent_class)->constructed (_object);

  path = utils_generate_object_path (BASE_DBUS_PATH, "Sysroot", NULL);
  self->sysroot = g_object_new (TYPE_SYSROOT,
      	                        "sysroot-path", self->sysroot_path,
                                NULL);

  if (!sysroot_populate (sysroot_get (), &error))
    {
      g_error ("Error setting up sysroot: %s", error->message);
      goto out;
    }

  daemon_publish (self, path, FALSE, self->sysroot);
  g_dbus_connection_start_message_processing (self->connection);

  g_debug ("daemon constructed");

out:
  g_clear_error (&error);
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
                                   PROP_SYSROOT_PATH,
                                   g_param_spec_string ("sysroot-path",
                                                         "Sysroot Path",
                                                         "Sysroot location on the filesystem",
                                                         FALSE,
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_ON_MESSAGE_BUS,
                                   g_param_spec_boolean ("on-message-bus",
                                                         "On Message Bus",
                                                         "Are we listining on the message bus",
                                                         TRUE,
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

gboolean
daemon_on_message_bus (Daemon *self)
{
  return self->on_message_bus;
}

void
daemon_hold (Daemon *self)
{
  self->use_count++;
}

void
daemon_release (Daemon *self)
{
  self->use_count--;

  if (self->use_count == 0)
    g_signal_emit (self, signals[FINISHED], 0);
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
