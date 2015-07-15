/*
* Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
* Copyright (C) 2013-2015 Red Hat, Inc.
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
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <syslog.h>
#include "libgsystem.h"

/* ---------------------------------------------------------------------------------------------------- */
static GMainLoop *loop = NULL;
static gboolean opt_debug = FALSE;
static char *opt_sysroot = "/";
static gint service_dbus_fd = -1;
static GOptionEntry opt_entries[] =
{
  {"debug", 'd', 0, G_OPTION_ARG_NONE, &opt_debug, "Print debug information on stderr", NULL},
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "dbus-peer", 0, 0, G_OPTION_ARG_INT, &service_dbus_fd, "Use a peer to peer dbus connection on this fd", NULL },
  {NULL }
};

static Daemon *rpm_ostree_daemon = NULL;

static void
on_close (Daemon *daemon, gpointer data)
{
  g_main_loop_quit (loop);
}

static void
start_daemon (GDBusConnection *connection,
              gboolean on_messsage_bus)
{
  GError *local_error = NULL;

  rpm_ostree_daemon = g_initable_new (TYPE_DAEMON, NULL, &local_error,
                                      "connection", connection,
                                      "sysroot-path", opt_sysroot,
                                      "on-message-bus", on_messsage_bus,
                                      NULL);

  if (local_error != NULL)
    {
      g_error ("%s", local_error->message);
      g_assert_not_reached ();
    }

  daemon_hold (rpm_ostree_daemon);

  g_signal_connect (rpm_ostree_daemon, "finished",
                    G_CALLBACK (on_close), NULL);
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char *name,
                 gpointer user_data)
{
  g_debug ("Connected to the system bus");
}


static void
on_name_acquired (GDBusConnection *connection,
                  const char *name,
                  gpointer user_data)
{
  g_debug ("Acquired the name %s on the system bus", name);

  start_daemon (connection, TRUE);
}

static void
on_name_lost (GDBusConnection *connection,
              const char *name,
              gpointer user_data)
{
  g_critical ("Lost (or failed to acquire) the "
              "name %s on the system bus", name);

  if (rpm_ostree_daemon == NULL)
    {
      g_main_loop_quit (loop);
    }
  else
    {
      daemon_release (rpm_ostree_daemon);
    }
}

static void
on_peer_acquired (GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
  GDBusConnection *connection;
  GError *error = NULL;

  connection = g_dbus_connection_new_finish (result, &error);
  if (error != NULL)
    {
      g_warning ("Couldn't connect to peer: %s", error->message);
      g_main_loop_quit (loop);
      g_error_free (error);
    }
  else
    {
      g_debug ("connected to peer");
      start_daemon (connection, FALSE);
    }
}


static gboolean
on_sigint (gpointer user_data)
{
  g_info ("Caught signal. Initiating shutdown");
  g_main_loop_quit (loop);
  return FALSE;
}


static gboolean
on_stdin_close (GIOChannel *channel,
                GIOCondition condition,
                gpointer data)
{
  /* Nowhere to log */
  syslog (LOG_INFO, "%s", "output closed");
  g_main_loop_quit (loop);
  return FALSE;
}


static void
on_log_debug (const gchar *log_domain,
             GLogLevelFlags log_level,
             const gchar *message,
             gpointer user_data)
{
  GString *string;
  const gchar *progname;
  const gchar *level;

  string = g_string_new (NULL);
  switch (log_level & G_LOG_LEVEL_MASK)
    {
      case G_LOG_LEVEL_DEBUG:
        level = "DEBUG";
        break;
      case G_LOG_LEVEL_INFO:
        level = "INFO";
        break;
      default:
        level = "";
        break;
    }


  progname = g_get_prgname ();
  if (progname == NULL)
    progname = "process";

  if (message == NULL)
    message = "(NULL) message";

  g_string_append_printf (string, "(%s:%lu): ", progname, (gulong) getpid ());

  if (log_domain != NULL)
    g_string_append_printf (string, "%s-", log_domain);

  g_string_append_printf (string, "%s: %s", level, message);

  g_printerr ("%s\n", string->str);

  g_string_free (string, TRUE);
}


static void
on_log_handler (const gchar *log_domain,
                GLogLevelFlags log_level,
                const gchar *message,
                gpointer user_data)
{
  static gboolean have_called_openlog = FALSE;
  const gchar *domains;
  int priority;

  if (!have_called_openlog)
    {
      have_called_openlog = TRUE;
      openlog (G_LOG_DOMAIN, LOG_CONS | LOG_NDELAY | LOG_PID, LOG_DAEMON);
    }

  /*
  * Note: we should not call GLib fucntions here.
  *
  * Mapping glib log levels to syslog priorities
  * is not at all obvious.
  */
  switch (log_level & G_LOG_LEVEL_MASK)
    {
    /*
    * In GLib this is always fatal, caller of this
    * function aborts()
    */

    case G_LOG_LEVEL_ERROR:
      priority = LOG_CRIT;
      break;

    /*
    * By convention in GLib applications, critical warnings
    * are usually internal programmer error (ie: precondition
    * failures). This maps well to LOG_CRIT.
    */
    case G_LOG_LEVEL_CRITICAL:
      priority = LOG_CRIT;
      break;

    /*
    * By convention in GLib apps, g_warning() is used for
    * non-fatal problems, but ones that should be corrected
    * or not be encountered in normal system behavior.
    */
    case G_LOG_LEVEL_WARNING:
      priority = LOG_WARNING;
      break;

    /*
    * These are related to bad input, or other hosts behaving
    * badly. Map well to syslog warnings.
    */
    case G_LOG_LEVEL_MESSAGE:
    default:
      priority = LOG_WARNING;
      break;

    /* Informational messages, startup, shutdown etc. */
    case G_LOG_LEVEL_INFO:
      priority = LOG_INFO;
      break;

    /* Debug messages. */
    case G_LOG_LEVEL_DEBUG:
      domains = g_getenv ("G_MESSAGES_DEBUG");
      if (domains == NULL ||
          (strcmp (domains, "all") != 0 && (!log_domain || !strstr (domains, log_domain))))
        return;

      priority = LOG_INFO;
      break;
    }

  syslog (priority, "%s", message);
}


static gboolean
connect_to_peer (int fd)
{
  gs_unref_object GSocketConnection *stream = NULL;
  gs_unref_object GSocket *socket = NULL;
  GError *error = NULL;
  gs_free gchar *guid = NULL;
  gboolean ret = FALSE;

  socket = g_socket_new_from_fd (fd, &error);
  if (error != NULL)
    {
      g_warning ("Couldn't create socket: %s", error->message);
      goto out;
    }

  stream = g_socket_connection_factory_create_connection (socket);
  if (!stream)
    {
      g_warning ("Couldn't create socket stream");
      goto out;
    }

  guid = g_dbus_generate_guid ();
  g_dbus_connection_new (G_IO_STREAM (stream), guid,
                         G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER |
                         G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING,
                         NULL, NULL, on_peer_acquired, NULL);
  ret = TRUE;

out:
  g_clear_error (&error);
  return ret;
}


int
main (int argc,
      char **argv)
{
  GError *error;
  GOptionContext *opt_context;
  GIOChannel *channel;
  guint name_owner_id = 0;
  gint ret;

  ret = 1;
  loop = NULL;
  opt_context = NULL;

  #if !GLIB_CHECK_VERSION(2,36,0)
  g_type_init ();
  #endif

  /* See glib/gio/gsocket.c */
  signal (SIGPIPE, SIG_IGN);

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  if (!g_setenv ("GIO_USE_VFS", "local", TRUE))
    {
      g_printerr ("Error setting GIO_USE_GVFS\n");
      goto out;
    }

  opt_context = g_option_context_new ("rpm-ostreed -- rpm-ostree daemon");
  g_option_context_add_main_entries (opt_context, opt_entries, NULL);
  error = NULL;
  if (!g_option_context_parse (opt_context, &argc, &argv, &error))
    {
      g_printerr ("Error parsing options: %s\n", error->message);
      g_error_free (error);
      goto out;
    }

  if (opt_debug)
    {
      g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO, on_log_debug, NULL);
      g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);

      /* When in debug mode (often testing) we exit when stdin closes */
      channel = g_io_channel_unix_new (0);
      g_io_add_watch (channel, G_IO_HUP, on_stdin_close, NULL);
      g_io_channel_unref (channel);
    }
  else
    {
      /* When not in debug mode, send all logging to syslog */
      g_log_set_default_handler (on_log_handler, NULL);
    }

  if (g_getenv ("PATH") == NULL)
    g_setenv ("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", TRUE);

  g_info ("rpm-ostreed starting");

  loop = g_main_loop_new (NULL, FALSE);

  g_unix_signal_add (SIGINT, on_sigint, NULL);
  g_unix_signal_add (SIGTERM, on_sigint, NULL);

  if (service_dbus_fd == -1)
    {
      name_owner_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                                      DBUS_NAME,
                                      G_BUS_NAME_OWNER_FLAGS_NONE,
                                      on_bus_acquired,
                                      on_name_acquired,
                                      on_name_lost,
                                      NULL, (GDestroyNotify) NULL);
    }
  else if (!connect_to_peer (service_dbus_fd))
    {
      ret = 1;
      goto out;
    }

  g_debug ("Entering main event loop");

  g_main_loop_run (loop);

  if (name_owner_id > 0)
    {
      g_bus_unown_name (name_owner_id);
      name_owner_id = 0;
    }

  g_clear_object (&rpm_ostree_daemon);

  ret = 0;

out:
  if (loop != NULL)
    g_main_loop_unref (loop);

  if (opt_context != NULL)
    g_option_context_free (opt_context);

  g_info ("rpm-ostreed exiting");

  return ret;
}
