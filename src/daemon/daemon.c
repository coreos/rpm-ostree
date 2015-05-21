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

#include "libgsystem.h"

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

  g_mutex_init (&self->mutex);
}

static void
daemon_constructed (GObject *_object)
{
  Daemon *self;
  GError *error = NULL;
  gs_free gchar *path = NULL;

  self = DAEMON (_object);
  self->object_manager = g_dbus_object_manager_server_new (BASE_DBUS_PATH);
  /* Export the ObjectManager */
  g_dbus_object_manager_server_set_connection (self->object_manager, self->connection);
  g_debug ("exported object manager");

  G_OBJECT_CLASS (daemon_parent_class)->constructed (_object);

  g_dbus_connection_start_message_processing (self->connection);

  g_debug ("daemon constructed");
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
