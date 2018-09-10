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
#include "rpmostree-util.h"

#include <libglnx.h>
#include <systemd/sd-journal.h>
#include <systemd/sd-login.h>
#include <systemd/sd-daemon.h>
#include <stdio.h>

#define RPMOSTREE_MESSAGE_TRANSACTION_STARTED SD_ID128_MAKE(d5,be,a3,7a,8f,c8,4f,f5,9d,bc,fd,79,17,7b,7d,f8)

#define RPMOSTREED_CONF SYSCONFDIR "/rpm-ostreed.conf"
#define DAEMON_CONFIG_GROUP "Daemon"
#define EXPERIMENTAL_CONFIG_GROUP "Experimental"

struct RpmOstreeClient;
static struct RpmOstreeClient *client_new (RpmostreedDaemon *self, const char *address, const char *id);

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
  GSource *idle_exit_source;
  guint rerender_status_id;
  RpmostreedSysroot *sysroot;
  gchar *sysroot_path;

  /* Settings from the config file */
  guint idle_exit_timeout;
  RpmostreedAutomaticUpdatePolicy auto_update_policy;

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
static void update_status (RpmostreedDaemon *self);

G_DEFINE_TYPE_WITH_CODE (RpmostreedDaemon, rpmostreed_daemon, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                rpmostreed_daemon_initable_iface_init))

struct RpmOstreeClient {
  char *id;
  char *address;
  guint name_watch_id;
  /* In Rust this'd be Option<uid_t> etc. */
  gboolean uid_valid;
  uid_t uid;
  gboolean pid_valid;
  pid_t pid;
  char *sd_unit;
};

static void
rpmostree_client_free (struct RpmOstreeClient *client)
{
  if (!client)
    return;
  g_free (client->id);
  g_free (client->address);
  g_free (client->sd_unit);
  g_free (client);
}

static char *
rpmostree_client_to_string (struct RpmOstreeClient *client)
{
  g_autoptr(GString) buf = g_string_new ("client(");
  if (client->id)
    g_string_append_printf (buf, "id:%s ", client->id);
  /* Since DBus addresses have a leading ':', let's avoid another. Yeah it's not
   * symmetric, but it does read better.
   */
  g_string_append (buf, "dbus");
  g_string_append (buf, client->address);
  if (client->sd_unit)
    g_string_append_printf (buf, " unit:%s", client->sd_unit);
  if (client->uid_valid)
    g_string_append_printf (buf, " uid:%lu", (unsigned long) client->uid);
  else
    g_string_append (buf, " uid:<unknown>");
  g_string_append_c (buf, ')');
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
  g_clear_pointer (&self->idle_exit_source, (GDestroyNotify)g_source_unref);
  if (self->rerender_status_id > 0)
    g_source_remove (self->rerender_status_id);

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
          struct RpmOstreeClient *clientdata = g_hash_table_lookup (self->bus_clients, sender);
          struct RpmOstreeClient *clientdata_owned = NULL;
          /* If the caller didn't register (e.g. Cockpit doesn't today),
           * then let's gather the relevant client info now.
           */
          if (!clientdata)
            clientdata = clientdata_owned = client_new (self, sender, NULL);
          g_autofree char *client_str = rpmostree_client_to_string (clientdata);
          g_autofree char *client_data_msg = NULL;
          if (clientdata->uid_valid)
            client_data_msg = g_strdup_printf ("CLIENT_UID=%u", clientdata->uid);
          /* TODO: convert other client fields to structured log */
          sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_TRANSACTION_STARTED),
                           "MESSAGE=Initiated txn %s for %s: %s", method, client_str, path,
                           "BUS_ADDRESS=%s", sender,
                           client_data_msg ?: NULL,
                           NULL);
          rpmostree_client_free (clientdata_owned);
        }
    }

  update_status (self);
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

  /* do this early so sysroot startup sets properties to the right values */
  if (!rpmostreed_daemon_reload_config (self, NULL, error))
    return FALSE;

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

/* Returns TRUE if config file exists and could be loaded or if config file doesn't exist.
 * Returns FALSE if config file exists but could not be loaded. */
static gboolean
maybe_load_config_keyfile (GKeyFile **out_keyfile,
                           GError   **error)
{
  g_autoptr(GError) local_error = NULL;

  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  if (g_key_file_load_from_file (keyfile, RPMOSTREED_CONF, 0, &local_error))
    {
      *out_keyfile = g_steal_pointer (&keyfile);
      sd_journal_print (LOG_INFO, "Reading config file '%s'", RPMOSTREED_CONF);
      return TRUE;
    }

  if (!g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  sd_journal_print (LOG_WARNING, "Missing config file '%s'; using compiled defaults",
                    RPMOSTREED_CONF);
  return TRUE;
}

static char*
get_config_str (GKeyFile   *keyfile,
                const char *key,
                const char *default_val)
{
  g_autofree char *val = NULL;
  if (keyfile)
    val = g_key_file_get_string (keyfile, DAEMON_CONFIG_GROUP, key, NULL);
  return g_steal_pointer (&val) ?: g_strdup (default_val);
}

static guint64
get_config_uint64 (GKeyFile   *keyfile,
                   const char *key,
                   guint64     default_val)
{
  if (keyfile && g_key_file_has_key (keyfile, DAEMON_CONFIG_GROUP, key, NULL))
    {
      g_autoptr(GError) local_error = NULL;
      guint64 r = g_key_file_get_uint64 (keyfile, DAEMON_CONFIG_GROUP, key, &local_error);
      if (!local_error)
        return r;
      if (g_error_matches (local_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE))
        sd_journal_print (LOG_WARNING, "Bad uint64 for '%s': %s; using compiled defaults",
                          key, local_error->message);
    }
  return default_val;
}

RpmostreedAutomaticUpdatePolicy
rpmostreed_get_automatic_update_policy (RpmostreedDaemon *self)
{
  return self->auto_update_policy;
}

/* in-place version of g_ascii_strdown */
static inline void
ascii_strdown_inplace (char *str)
{
  for (char *c = str; *c; c++)
    *c = g_ascii_tolower (*c);
}

gboolean
rpmostreed_daemon_reload_config (RpmostreedDaemon *self,
                                 gboolean         *out_changed,
                                 GError          **error)
{
  g_autoptr(GKeyFile) config = NULL;
  if (!maybe_load_config_keyfile (&config, error))
    return FALSE;

  /* default to 60s by default; our startup is non-trivial so staying around will ensure
   * follow-up requests are more responsive */
  guint64 idle_exit_timeout = get_config_uint64 (config, "IdleExitTimeout", 60);

  /* default to off for now; we will change it to "check" in a later release */
  RpmostreedAutomaticUpdatePolicy auto_update_policy =
    RPMOSTREED_AUTOMATIC_UPDATE_POLICY_NONE;

  g_autofree char *auto_update_policy_str =
    get_config_str (config, "AutomaticUpdatePolicy", NULL);
  if (auto_update_policy_str)
    {
      ascii_strdown_inplace (auto_update_policy_str);
      if (!rpmostree_str_to_auto_update_policy (auto_update_policy_str,
                                                &auto_update_policy, error))
        return FALSE;
    }

  /* don't update changed for this; it's contained to RpmostreedDaemon so no other objects
   * need to be reloaded if it changes */
  self->idle_exit_timeout = idle_exit_timeout;

  gboolean changed = FALSE;

  changed = changed || (self->auto_update_policy != auto_update_policy);

  self->auto_update_policy = auto_update_policy;

  if (out_changed)
    *out_changed = changed;
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

static gboolean
idle_update_status (void *data)
{
  RpmostreedDaemon *self = data;

  update_status (self);

  return TRUE;
}


static gboolean
on_idle_exit (void *data)
{
  RpmostreedDaemon *self = data;

  g_clear_pointer (&self->idle_exit_source, (GDestroyNotify)g_source_unref);

  sd_notifyf (0, "STATUS=Exiting due to idle");
  self->running = FALSE;
  g_main_context_wakeup (NULL);

  return FALSE;
}

static void
update_status (RpmostreedDaemon *self)
{
  g_autoptr(GVariant) active_txn = NULL;
  gboolean have_active_txn = FALSE;
  const char *method, *sender, *path;
  const guint n_clients = g_hash_table_size (self->bus_clients);
  gboolean currently_idle = FALSE;

  g_object_get (rpmostreed_sysroot_get (), "active-transaction", &active_txn, NULL);

  if (active_txn)
    {
      g_variant_get (active_txn, "(&s&s&s)", &method, &sender, &path);
      if (*method)
        have_active_txn = TRUE;
    }

  if (!getenv ("RPMOSTREE_DEBUG_DISABLE_DAEMON_IDLE_EXIT") && self->idle_exit_timeout > 0)
    currently_idle = !have_active_txn && n_clients == 0;

  if (currently_idle && !self->idle_exit_source)
    {
      /* I think adding some randomness is a good idea, to mitigate
       * pathological cases where someone is talking to us at the same
       * frequency as our exit timer. */
      const guint idle_exit_secs = self->idle_exit_timeout + g_random_int_range (0, 5);
      self->idle_exit_source = g_timeout_source_new_seconds (idle_exit_secs);
      g_source_set_callback (self->idle_exit_source, on_idle_exit, self, NULL);
      g_source_attach (self->idle_exit_source, NULL);
      /* This source ensures we update the systemd status to show admins
       * when we may auto-exit.
       */
      self->rerender_status_id = g_timeout_add_seconds (1, idle_update_status, self);
      sd_journal_print (LOG_INFO, "In idle state; will auto-exit in %u seconds", idle_exit_secs);
    }
  else if (!currently_idle && self->idle_exit_source)
    {
      g_source_destroy (self->idle_exit_source);
      g_clear_pointer (&self->idle_exit_source, (GDestroyNotify)g_source_unref);
      g_source_remove (self->rerender_status_id);
    }

  if (have_active_txn)
    {
      sd_notifyf (0, "STATUS=clients=%u; txn=%s caller=%s path=%s", g_hash_table_size (self->bus_clients),
                  method, sender, path);
    }
  else if (n_clients > 0)
    sd_notifyf (0, "STATUS=clients=%u; idle", n_clients);
  else if (currently_idle)
    {
      guint64 readytime = g_source_get_ready_time (self->idle_exit_source);
      guint64 curtime = g_source_get_time (self->idle_exit_source);
      guint64 timeout_micros = readytime - curtime;
      if (readytime < curtime)
        timeout_micros = 0;

      g_assert (currently_idle && self->idle_exit_source);
      sd_notifyf (0, "STATUS=clients=%u; idle exit in %" G_GUINT64_FORMAT " seconds", n_clients,
                  timeout_micros / G_USEC_PER_SEC);
    }
}

gboolean
rpmostreed_get_client_uid (RpmostreedDaemon *self,
                           const char       *client,
                           uid_t            *out_uid)
{
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
      return FALSE;
    }

  g_variant_get (uidcall, "(u)", out_uid);
  return TRUE;
}

static gboolean
get_client_pid (RpmostreedDaemon *self,
                const char       *client,
                pid_t            *out_pid)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GVariant) all = g_dbus_proxy_call_sync (self->bus_proxy,
                                                    "GetConnectionUnixProcessID",
                                                    g_variant_new ("(s)", client),
                                                    G_DBUS_CALL_FLAGS_NONE,
                                                    2000, NULL, &local_error);
  if (!all)
    {
      sd_journal_print (LOG_WARNING, "Failed to GetConnectionUnixProcessID for client %s: %s",
                        client, local_error->message);
      return FALSE;
    }

  g_variant_get (all, "(u)", out_pid);
  return TRUE;
}

/* Given a DBus address, load metadata for it */
static struct RpmOstreeClient *
client_new (RpmostreedDaemon *self, const char *address,
            const char *client_id)
{
  struct RpmOstreeClient *client = g_new0 (struct RpmOstreeClient, 1);
  client->address = g_strdup (address);
  client->id = g_strdup (client_id);
  if (rpmostreed_get_client_uid (self, address, &client->uid))
    client->uid_valid = TRUE;
  if (get_client_pid (self, address, &client->pid))
    {
      client->pid_valid = TRUE;
      if (sd_pid_get_user_unit (client->pid, &client->sd_unit) == 0)
        {}
      else if (sd_pid_get_unit (client->pid, &client->sd_unit) == 0)
        {}
    }

  return g_steal_pointer (&client);
}

void
rpmostreed_daemon_add_client (RpmostreedDaemon *self,
                              const char       *client,
                              const char       *client_id)
{
  if (g_hash_table_lookup (self->bus_clients, client))
    return;

  struct RpmOstreeClient *clientdata = client_new (self, client, client_id);
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

  g_hash_table_insert (self->bus_clients, (char*)clientdata->address, clientdata);
  g_autofree char *clientstr = rpmostree_client_to_string (clientdata);
  sd_journal_print (LOG_INFO, "%s added; new total=%u", clientstr, g_hash_table_size (self->bus_clients));
  update_status (self);
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
  const guint remaining = g_hash_table_size (self->bus_clients);
  sd_journal_print (LOG_INFO, "%s vanished; remaining=%u", clientstr, remaining);
  update_status (self);
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
  update_status (self);
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
