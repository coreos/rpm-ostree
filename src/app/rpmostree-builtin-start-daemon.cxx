/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

#include "libglnx.h"
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <libglnx.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-journal.h>

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-util.h"
#include "rpmostreed-daemon.h"

typedef enum
{
  APPSTATE_STARTING, /* Before the ♫♫♫ maaaain event ♫♫♫ */
  APPSTATE_RUNNING,  /* Main event loop */
  APPSTATE_FLUSHING, /* We should release our bus name, and wait for it to be released */
  APPSTATE_EXITING,  /* About to exit() */
} AppState;

static AppState appstate = APPSTATE_STARTING;
static gboolean opt_debug = FALSE;
static char *opt_sysroot = NULL;
static GOptionEntry opt_entries[] = { { "debug", 'd', 0, G_OPTION_ARG_NONE, &opt_debug,
                                        "Print debug information on stderr", NULL },
                                      { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot,
                                        "Use system root SYSROOT (default: /)", "SYSROOT" },
                                      { NULL } };

static RpmostreedDaemon *rpm_ostree_daemon = NULL;

static void
state_transition (AppState state)
{
  g_assert_cmpint (state, >, appstate);
  appstate = state;
  if (state > APPSTATE_RUNNING && rpm_ostree_daemon)
    rpmostreed_daemon_exit_now (rpm_ostree_daemon);
  g_main_context_wakeup (NULL);
}

static gboolean
start_daemon (GDBusConnection *connection, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Couldn't start daemon", error);
  rpm_ostree_daemon
      = (RpmostreedDaemon *)g_initable_new (RPMOSTREED_TYPE_DAEMON, NULL, error, "connection",
                                            connection, "sysroot-path", opt_sysroot ?: "/", NULL);
  if (rpm_ostree_daemon == NULL)
    return FALSE;
  (void)g_bus_own_name_on_connection (connection, DBUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL,
                                      NULL, NULL, NULL);
  return TRUE;
}

static void
on_bus_name_released (GDBusConnection *connection, GAsyncResult *result, void *user_data)
{
  state_transition (APPSTATE_EXITING);
}

static gboolean
on_sigint (gpointer user_data)
{
  state_transition (APPSTATE_FLUSHING);
  sd_notify (FALSE, "STATUS=Received shutdown signal, preparing to terminate");
  return FALSE;
}

static gboolean
on_stdin_close (GIOChannel *channel, GIOCondition condition, gpointer data)
{
  /* Nowhere to log */
  sd_journal_print (LOG_INFO, "%s", "output closed");
  state_transition (APPSTATE_FLUSHING);
  return FALSE;
}

static void
on_log_debug (const gchar *log_domain, GLogLevelFlags log_level, const gchar *message,
              gpointer user_data)
{
  g_autoptr (GString) string = NULL;
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

  g_string_append_printf (string, "(%s:%lu): ", progname, (gulong)getpid ());

  if (log_domain != NULL)
    g_string_append_printf (string, "%s-", log_domain);

  g_string_append_printf (string, "%s: %s", level, message);

  g_printerr ("%s\n", string->str);
}

static void
on_log_handler (const gchar *log_domain, GLogLevelFlags log_level, const gchar *message,
                gpointer user_data)
{
  const gchar *domains;
  int priority;

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
      if (domains == NULL
          || (strcmp (domains, "all") != 0 && (!log_domain || !strstr (domains, log_domain))))
        return;

      priority = LOG_INFO;
      break;
    }

  sd_journal_print (priority, "%s", message);
}

gboolean
rpmostree_builtin_start_daemon (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                                GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) opt_context = g_option_context_new (" - start the daemon process");
  g_option_context_add_main_entries (opt_context, opt_entries, NULL);

  if (!g_option_context_parse (opt_context, &argc, &argv, error))
    return FALSE;

  if (opt_debug)
    {
      g_autoptr (GIOChannel) channel = NULL;
      g_log_set_handler (G_LOG_DOMAIN, (GLogLevelFlags)(G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO),
                         on_log_debug, NULL);
      g_log_set_always_fatal (
          (GLogLevelFlags)(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

      /* When in debug mode (often testing) we exit when stdin closes */
      channel = g_io_channel_unix_new (0);
      g_io_add_watch (channel, G_IO_HUP, on_stdin_close, NULL);
    }
  else
    {
      /* When not in debug mode, send all logging to syslog */
      g_log_set_default_handler (on_log_handler, NULL);
    }

  g_unix_signal_add (SIGINT, on_sigint, NULL);
  g_unix_signal_add (SIGTERM, on_sigint, NULL);

  /* Get an explicit ref to the bus so we can use it later */
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
  if (!bus)
    return FALSE;
  if (!start_daemon (bus, error))
    {
      if (*error)
        sd_notifyf (0, "STATUS=error: %s", (*error)->message);
      return FALSE;
    }

  state_transition (APPSTATE_RUNNING);

  g_debug ("Entering main event loop");
  rpmostreed_daemon_run_until_idle_exit (rpm_ostree_daemon);

  if (bus)
    {
      /* We first tell systemd we're stopping, so it knows to activate a new instance
       * and avoid sending any more traffic our way.
       * After that, release the name via API directly so we can wait for the result.
       * More info:
       *  https://github.com/projectatomic/rpm-ostree/pull/606
       *  https://lists.freedesktop.org/archives/dbus/2015-May/016671.html
       *  https://github.com/cgwalters/test-exit-on-idle
       */
      sd_notify (FALSE, "STOPPING=1");
      /* The rpmostreed_daemon_run_until_idle_exit() path won't actually set
       * FLUSHING right now, let's just forcibly do so if it hasn't been done
       * already.
       */
      if (appstate < APPSTATE_FLUSHING)
        state_transition (APPSTATE_FLUSHING);
      g_dbus_connection_call (
          bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
          "ReleaseName", g_variant_new ("(s)", DBUS_NAME), G_VARIANT_TYPE ("(u)"),
          G_DBUS_CALL_FLAGS_NONE, -1, NULL, (GAsyncReadyCallback)on_bus_name_released, NULL);
    }

  /* Waiting 🛌 for the name to be released */
  g_autoptr (GMainContext) mainctx = g_main_context_default ();
  while (appstate == APPSTATE_FLUSHING)
    g_main_context_iteration (mainctx, TRUE);

  return TRUE;
}
