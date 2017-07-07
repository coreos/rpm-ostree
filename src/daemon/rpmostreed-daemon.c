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

#include "rpmostreed-daemon.h"
#include "rpmostreed-sysroot.h"
#include "rpmostreed-types.h"
#include "rpmostreed-utils.h"

#include <libglnx.h>
#include <systemd/sd-journal.h>
#include <systemd/sd-daemon.h>
#include <stdio.h>

#define RPMOSTREE_MESSAGE_TRANSACTION_STARTED SD_ID128_MAKE(d5,be,a3,7a,8f,c8,4f,f5,9d,bc,fd,79,17,7b,7d,f8)

/**
 * SECTION: daemon
 * @title: RpmostreedDaemon
 * @short_description: Main daemon object
 *
 * Object holding all global state.
 */

typedef struct _RpmostreedDaemonClass RpmostreedDaemonClass;

/**
 * RpmostreedDaemon:
 *
 * The #RpmostreedDaemon structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _RpmostreedDaemon {
  GObject parent_instance;

  GHashTable *bus_clients; /* <utf8 busname, struct RpmOstreeClient> */

  gboolean running;
  GDBusProxy *bus_proxy;
  RpmostreedSysroot *sysroot;
  gchar *sysroot_path;

  GDBusConnection *connection;
  GDBusObjectManagerServer *object_manager;
};

struct _RpmostreedDaemonClass {
  GObjectClass parent_class;
};

static RpmostreedDaemon *_daemon_instance;

enum
{
  PROP_0,
  PROP_CONNECTION,
  PROP_OBJECT_MANAGER,
  PROP_SYSROOT_PATH
};

static void rpmostreed_daemon_initable_iface_init (GInitableIface *iface);
static void render_systemd_status (RpmostreedDaemon *self);

G_DEFINE_TYPE_WITH_CODE (RpmostreedDaemon, rpmostreed_daemon, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                rpmostreed_daemon_initable_iface_init))

struct RpmOstreeClient {
  char *address;
  guint name_watch_id;
  gboolean uid_valid;
  uid_t uid;
};

static void
rpmostree_client_free (struct RpmOstreeClient *client)
{
  g_free (client->address);
  g_free (client);
}

static char *
rpmostree_client_to_string (struct RpmOstreeClient *client)
{
  g_autoptr(GString) buf = g_string_new ("client ");
  g_string_append (buf, client->address);
  if (client->uid_valid)
    g_string_append_printf (buf, " (uid %lu)", (unsigned long) client->uid);
  else
    g_string_append (buf, " (unknown uid)");
  return g_string_free (g_steal_pointer (&buf), FALSE);
}

typedef struct {
  GAsyncReadyCallback callback;
  gpointer callback_data;
  gpointer proxy_source;
} DaemonTaskData;


static void
daemon_finalize (GObject *object)
{
  RpmostreedDaemon *self = RPMOSTREED_DAEMON (object);

  g_clear_object (&self->object_manager);
  self->object_manager = NULL;

  g_clear_object (&self->sysroot);
  g_clear_object (&self->bus_proxy);

  g_object_unref (self->connection);
  g_hash_table_unref (self->bus_clients);

  g_free (self->sysroot_path);
  G_OBJECT_CLASS (rpmostreed_daemon_parent_class)->finalize (object);

  _daemon_instance = NULL;
}


static void
daemon_get_property (GObject *object,
                     guint prop_id,
                     GValue *value,
                     GParamSpec *pspec)
{
  RpmostreedDaemon *self = RPMOSTREED_DAEMON (object);

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
  RpmostreedDaemon *self = RPMOSTREED_DAEMON (object);

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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
rpmostreed_daemon_init (RpmostreedDaemon *self)
{
  g_assert (_daemon_instance == NULL);
  _daemon_instance = self;

  self->sysroot_path = NULL;
  self->sysroot = NULL;
  self->bus_clients = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)rpmostree_client_free);
}

static void
on_active_txn_changed (GObject *object,
                       GParamSpec *pspec,
                       gpointer user_data)
{
  RpmostreedDaemon *self = user_data;
  g_autoptr(GVariant) active_txn = NULL;
  const char *method, *sender, *path;

  g_object_get (rpmostreed_sysroot_get (), "active-transaction", &active_txn, NULL);

  if (active_txn)
    {
      g_variant_get (active_txn, "(&s&s&s)", &method, &sender, &path);
      if (*method)
        {
          g_autofree char *client_str = rpmostreed_daemon_client_get_string (self, sender);
          struct RpmOstreeClient *clientdata = g_hash_table_lookup (self->bus_clients, sender);
          g_autofree char *client_data_msg = NULL;
          if (clientdata && clientdata->uid_valid)
            client_data_msg = g_strdup_printf ("CLIENT_UID=%u", clientdata->uid);
          sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_TRANSACTION_STARTED),
                           "MESSAGE=Initiated txn %s for %s: %s", method, client_str, path,
                           "BUS_ADDRESS=%s", sender,
                           client_data_msg ?: NULL,
                           NULL);
        }
    }

  render_systemd_status (self);
}

static gboolean
rpmostreed_daemon_initable_init (GInitable *initable,
                                 GCancellable *cancellable,
                                 GError **error)
{
  RpmostreedDaemon *self = RPMOSTREED_DAEMON (initable);

  self->object_manager = g_dbus_object_manager_server_new (BASE_DBUS_PATH);

  /* Export the ObjectManager */
  g_dbus_object_manager_server_set_connection (self->object_manager, self->connection);
  g_debug ("exported object manager");

  g_autofree gchar *path =
    rpmostreed_generate_object_path (BASE_DBUS_PATH, "Sysroot", NULL);
  self->sysroot = g_object_new (RPMOSTREED_TYPE_SYSROOT,
                                "path", self->sysroot_path,
                                NULL);

  if (!rpmostreed_sysroot_populate (rpmostreed_sysroot_get (), cancellable, error))
    return glnx_prefix_error (error, "Error setting up sysroot");

  g_signal_connect (rpmostreed_sysroot_get (), "notify::active-transaction",
                    G_CALLBACK (on_active_txn_changed), self);

  self->bus_proxy =
    g_dbus_proxy_new_sync (self->connection,
                           G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                           G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                           NULL,
                           "org.freedesktop.DBus",
                           "/org/freedesktop/DBus",
                           "org.freedesktop.DBus",
                           NULL,
                           error);
  if (!self->bus_proxy)
    return FALSE;

  rpmostreed_daemon_publish (self, path, FALSE, self->sysroot);
  g_dbus_connection_start_message_processing (self->connection);

  g_debug ("daemon constructed");

  return TRUE;
}

static void
rpmostreed_daemon_class_init (RpmostreedDaemonClass *klass)
{
  GObjectClass *gobject_class;
  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = daemon_finalize;
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
}

static void
rpmostreed_daemon_initable_iface_init (GInitableIface *iface)
{
  iface->init = rpmostreed_daemon_initable_init;
}

/**
 * rpmostreed_daemon_get:
 *
 * Returns: (transfer none): The singleton #RpmostreedDaemon instance
 */
RpmostreedDaemon *
rpmostreed_daemon_get (void)
{
  g_assert (_daemon_instance);
  return _daemon_instance;
}

static void
on_name_owner_changed (GDBusConnection  *connection,
                       const gchar      *sender_name,
                       const gchar      *object_path,
                       const gchar      *interface_name,
                       const gchar      *signal_name,
                       GVariant         *parameters,
                       gpointer          user_data)
{
  RpmostreedDaemon *self = user_data;
  const char *name;
  const char *old_owner;
  gboolean has_old_owner;
  const char *new_owner;
  gboolean has_new_owner;

  if (g_strcmp0 (object_path, "/org/freedesktop/DBus") != 0 ||
      g_strcmp0 (interface_name, "org.freedesktop.DBus") != 0 ||
      g_strcmp0 (sender_name, "org.freedesktop.DBus") != 0)
    return;

  g_variant_get (parameters, "(&s&s&s)", &name, &old_owner, &new_owner);

  has_old_owner = old_owner && *old_owner;
  has_new_owner = new_owner && *new_owner;
  if (has_old_owner && !has_new_owner)
    rpmostreed_daemon_remove_client (self, name);
}

static void
render_systemd_status (RpmostreedDaemon *self)
{
  g_autoptr(GVariant) active_txn = NULL;
  gboolean have_active_txn = FALSE;
  const char *method, *sender, *path;

  g_object_get (rpmostreed_sysroot_get (), "active-transaction", &active_txn, NULL);

  if (active_txn)
    {
      g_variant_get (active_txn, "(&s&s&s)", &method, &sender, &path);
      if (*method)
        have_active_txn = TRUE;
    }

  if (have_active_txn)
    {
      sd_notifyf (0, "STATUS=clients=%u; txn=%s caller=%s path=%s", g_hash_table_size (self->bus_clients),
                  method, sender, path);
    }
  else
    sd_notifyf (0, "STATUS=clients=%u; idle", g_hash_table_size (self->bus_clients));
}

void
rpmostreed_daemon_add_client (RpmostreedDaemon *self,
                              const char       *client)
{
  if (g_hash_table_lookup (self->bus_clients, client))
    return;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GVariant) uidcall = g_dbus_proxy_call_sync (self->bus_proxy,
                                                        "GetConnectionUnixUser",
                                                        g_variant_new ("(s)", client),
                                                        G_DBUS_CALL_FLAGS_NONE,
                                                        2000, NULL, &local_error);
  if (!uidcall)
    {
      sd_journal_print (LOG_WARNING, "Failed to GetConnectionUnixUser for client %s: %s",
                        client, local_error->message);
      g_clear_error (&local_error);
    }

  struct RpmOstreeClient *clientdata = g_new0 (struct RpmOstreeClient, 1);
  clientdata->address = g_strdup (client);
  clientdata->name_watch_id =
    g_dbus_connection_signal_subscribe (self->connection,
                                        "org.freedesktop.DBus",
                                        "org.freedesktop.DBus",
                                        "NameOwnerChanged",
                                        "/org/freedesktop/DBus",
                                        client,
                                        G_DBUS_SIGNAL_FLAGS_NONE,
                                        on_name_owner_changed,
                                        g_object_ref (self),
                                        g_object_unref);
  if (uidcall)
    {
      g_variant_get (uidcall, "(u)", &clientdata->uid);
      clientdata->uid_valid = TRUE;
    }

  g_hash_table_insert (self->bus_clients, (char*)clientdata->address, clientdata);
  g_autofree char *clientstr = rpmostree_client_to_string (clientdata);
  sd_journal_print (LOG_INFO, "%s added; new total=%u", clientstr, g_hash_table_size (self->bus_clients));
  render_systemd_status (self);
}

/* Returns a string representing the state of the bus name @client.
 * If @client is unknown (i.e. has not called RegisterClient), we just
 * return "caller".
 */
char *
rpmostreed_daemon_client_get_string (RpmostreedDaemon *self, const char *client)
{
  struct RpmOstreeClient *clientdata = g_hash_table_lookup (self->bus_clients, client);
  if (!clientdata)
    return g_strdup_printf ("caller %s", client);
  else
    return rpmostree_client_to_string (clientdata);
}

void
rpmostreed_daemon_remove_client (RpmostreedDaemon *self,
                                 const char       *client)
{
  gpointer origkey, clientdatap;
  struct RpmOstreeClient *clientdata;
  if (!g_hash_table_lookup_extended (self->bus_clients, client, &origkey, &clientdatap))
    return;
  clientdata = clientdatap;
  g_dbus_connection_signal_unsubscribe (self->connection, clientdata->name_watch_id);
  g_autofree char *clientstr = rpmostree_client_to_string (clientdata);
  g_hash_table_remove (self->bus_clients, client);
  sd_journal_print (LOG_INFO, "%s vanished; remaining=%u", clientstr, g_hash_table_size (self->bus_clients));
  render_systemd_status (self);
}

void
rpmostreed_daemon_exit_now (RpmostreedDaemon *self)
{
  self->running = FALSE;
}

void
rpmostreed_daemon_run_until_idle_exit (RpmostreedDaemon *self)
{
  self->running = TRUE;
  render_systemd_status (self);
  while (self->running)
    g_main_context_iteration (NULL, TRUE);
}

void
rpmostreed_daemon_publish (RpmostreedDaemon *self,
                           const gchar *path,
                           gboolean uniquely,
                           gpointer thing)
{
  GDBusInterface *prev = NULL;
  GDBusInterfaceInfo *info = NULL;
  GDBusObjectSkeleton *object = NULL;
  g_autoptr(GDBusObjectSkeleton) owned_object = NULL;

  g_return_if_fail (RPMOSTREED_IS_DAEMON (self));
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
        object = owned_object = g_dbus_object_skeleton_new (path);

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
}

void
rpmostreed_daemon_unpublish (RpmostreedDaemon *self,
                             const gchar *path,
                             gpointer thing)
{
  GDBusObject *object;
  gboolean unexport = FALSE;
  GList *interfaces, *l;

  g_return_if_fail (RPMOSTREED_IS_DAEMON (self));
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
