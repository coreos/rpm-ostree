/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <gio/gio.h>
#include <glib-unix.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>

#include "rpmostree-builtins.h"
#include "rpmostree-polkit-agent.h"

#include "libglnx.h"

static RpmOstreeCommand commands[] = {
#ifdef HAVE_COMPOSE_TOOLING
  { "compose", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
               RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    "Commands to compose a tree",
    rpmostree_builtin_compose },
#endif
  { "cleanup", 0,
    "Clear cached/pending data",
    rpmostree_builtin_cleanup },
  { "db", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Commands to query the RPM database",
    rpmostree_builtin_db },
  { "deploy", RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS,
    "Deploy a specific commit",
    rpmostree_builtin_deploy },
  { "rebase", RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS,
    "Switch to a different tree",
    rpmostree_builtin_rebase },
  { "rollback", 0,
    "Revert to the previously booted tree",
    rpmostree_builtin_rollback },
  { "status", 0,
    "Get the version of the booted system",
    rpmostree_builtin_status },
  { "upgrade", RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS,
    "Perform a system upgrade",
    rpmostree_builtin_upgrade },
  { "reload", 0,
    "Reload configuration",
    rpmostree_builtin_reload },
  { "initramfs", 0,
    "Enable or disable local initramfs regeneration",
    rpmostree_builtin_initramfs },
  { "install", 0,
    "Download and install layered RPM packages",
    rpmostree_builtin_install },
  { "uninstall", 0,
    "Remove one or more overlay packages",
    rpmostree_builtin_uninstall },
  { "override", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Manage base overrides", rpmostree_builtin_override },
  { "refresh-md", 0,
    "Generate rpm repo metadata",
    rpmostree_builtin_refresh_md },
  /* Legacy aliases */
  { "pkg-add", RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    NULL, rpmostree_builtin_install },
  { "pkg-remove", RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    NULL, rpmostree_builtin_uninstall },
  { "rpm", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
           RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    NULL, rpmostree_builtin_db },
  { "makecache", RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    NULL, rpmostree_builtin_refresh_md },
  /* Hidden */
  { "ex", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
          RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    "Commands still under experiment", rpmostree_builtin_ex },
  { "start-daemon", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
                    RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT |
                    RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    NULL, rpmostree_builtin_start_daemon },
  { NULL }
};

static gboolean opt_version;
static gboolean opt_force_peer;
static char *opt_sysroot;
static gchar **opt_install;
static gchar **opt_uninstall;

static GOptionEntry global_entries[] = {
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { NULL }
};

static GOptionEntry daemon_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "peer", 0, 0, G_OPTION_ARG_NONE, &opt_force_peer, "Force a peer-to-peer connection instead of using the system message bus", NULL },
  { NULL }
};

static GOptionEntry pkg_entries[] = {
  { "install", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_install, "Install a package", "PKG" },
  { "uninstall", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_uninstall, "Uninstall a package", "PKG" },
  { NULL }
};

static GOptionContext *
option_context_new_with_commands (RpmOstreeCommandInvocation *invocation,
                                  RpmOstreeCommand *commands)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("COMMAND");
  g_autoptr(GString) summary = g_string_new (NULL);

  if (invocation)
    {
      if (invocation->command->description != NULL)
        g_string_append_printf (summary, "%s\n\n",
                                invocation->command->description);

      g_string_append_printf (summary, "Builtin \"%s\" Commands:",
                              invocation->command->name);
    }
  else /* top level */
    g_string_append (summary, "Builtin Commands:");

  for (RpmOstreeCommand *command = commands; command->name != NULL; command++)
    {
      gboolean hidden = (command->flags & RPM_OSTREE_BUILTIN_FLAG_HIDDEN) > 0;
      if (!hidden)
        {
          g_string_append_printf (summary, "\n  %-17s", command->name);
          if (command->description != NULL)
            g_string_append_printf (summary, "%s", command->description);
        }
    }

  g_option_context_set_summary (context, summary->str);
  return g_steal_pointer (&context);
}

gboolean
rpmostree_option_context_parse (GOptionContext *context,
                                const GOptionEntry *main_entries,
                                int *argc,
                                char ***argv,
                                RpmOstreeCommandInvocation *invocation,
                                GCancellable *cancellable,
                                const char *const* *out_install_pkgs,
                                const char *const* *out_uninstall_pkgs,
                                RPMOSTreeSysroot **out_sysroot_proxy,
                                GPid *out_peer_pid,
                                GError **error)
{
  /* with --version there's no command, don't require a daemon for it */
  const RpmOstreeBuiltinFlags flags =
    invocation ? invocation->command->flags : RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD;
  gboolean use_daemon = ((flags & RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD) == 0);

  if (invocation && invocation->command->description != NULL)
    {
      /* The extra summary explanation is only provided for commands with description */
      const char* context_summary = g_option_context_get_summary (context);

      /* check whether the summary has been set earlier */
      if (context_summary == NULL)
       g_option_context_set_summary (context, invocation->command->description);
    }

  if (main_entries != NULL)
    g_option_context_add_main_entries (context, main_entries, NULL);

  if (use_daemon)
    g_option_context_add_main_entries (context, daemon_entries, NULL);

  if ((flags & RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS) > 0)
    g_option_context_add_main_entries (context, pkg_entries, NULL);

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!g_option_context_parse (context, argc, argv, error))
    return FALSE;

  if (opt_version)
    {
      /* This should now be YAML, like `docker version`, so it's both nice to read
       * possible to parse.  The canonical implementation of this is in
       * ostree/ot-main.c.
       */
      g_auto(GStrv) features = g_strsplit (RPM_OSTREE_FEATURES, " ", -1);
      g_print ("%s:\n", PACKAGE_NAME);
      g_print (" Version: %s\n", PACKAGE_VERSION);
      if (strlen (RPM_OSTREE_GITREV) > 0)
        g_print (" Git: %s\n", RPM_OSTREE_GITREV);
      g_print (" Features:\n");
      for (char **iter = features; iter && *iter; iter++)
        g_print ("  - %s\n", *iter);
      exit (EXIT_SUCCESS);
    }

  if ((flags & RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT) > 0
      && getuid () != 0
      && getenv ("RPMOSTREE_SUPPRESS_REQUIRES_ROOT_CHECK") == NULL)
    return glnx_throw (error, "This command requires root privileges");

  if (use_daemon)
    {
      /* root never needs to auth */
      if (getuid () != 0)
        /* ignore errors; we print out a warning if we fail to spawn pkttyagent */
        (void)rpmostree_polkit_agent_open ();

      if (!rpmostree_load_sysroot (opt_sysroot,
                                   opt_force_peer,
                                   cancellable,
                                   out_sysroot_proxy,
                                   out_peer_pid,
                                   error))
        return FALSE;
    }

  if (out_install_pkgs)
    *out_install_pkgs = (const char *const*)opt_install;
  if (out_uninstall_pkgs)
    *out_uninstall_pkgs = (const char *const*)opt_uninstall;

  return TRUE;
}

static RpmOstreeCommand *
lookup_command (const char *name)
{
  RpmOstreeCommand *command = commands;

  while (command->name)
    {
      if (g_strcmp0 (name, command->name) == 0)
        return command;
      command++;
    }
  return NULL;
}

const char *
rpmostree_subcommand_parse (int *inout_argc,
                            char **inout_argv,
                            RpmOstreeCommandInvocation *invocation)
{
  const int argc = *inout_argc;
  const char *command_name = NULL;
  int in, out;

  for (in = 1, out = 1; in < argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (inout_argv[in][0] != '-')
        {
          if (command_name == NULL)
            {
              command_name = inout_argv[in];
              out--;
              continue;
            }
        }

      else if (g_str_equal (inout_argv[in], "--"))
        {
          break;
        }

      inout_argv[out] = inout_argv[in];
    }

  *inout_argc = out;
  return command_name;
}

int
rpmostree_handle_subcommand (int argc, char **argv,
                             RpmOstreeCommand *subcommands,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable *cancellable, GError **error)
{
  const char *subcommand_name =
    rpmostree_subcommand_parse (&argc, argv, invocation);

  RpmOstreeCommand *subcommand = subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      g_autoptr(GOptionContext) context =
        option_context_new_with_commands (invocation, subcommands);

      /* This will not return for some options (e.g. --version). */
      (void) rpmostree_option_context_parse (context, NULL,
                                             &argc, &argv,
                                             invocation,
                                             cancellable,
                                             NULL, NULL, NULL, NULL, NULL);
      if (subcommand_name == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No \"%s\" subcommand specified",
                       invocation->command->name);
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unknown \"%s\" subcommand \"%s\"",
                       invocation->command->name,
                       subcommand_name);
        }

      g_autofree char *help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);
      return EXIT_FAILURE;
    }

  g_autofree char *prgname =
    g_strdup_printf ("%s %s", g_get_prgname (), subcommand_name);
  g_set_prgname (prgname);

  RpmOstreeCommandInvocation sub_invocation = { .command = subcommand };
  return subcommand->fn (argc, argv, &sub_invocation, cancellable, error);
}

int
main (int    argc,
      char **argv)
{
  GCancellable *cancellable = g_cancellable_new ();
  RpmOstreeCommand *command;
  int exit_status = EXIT_SUCCESS;
  const char *command_name = NULL;
  g_autofree char *prgname = NULL;
  GError *local_error = NULL;

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  g_setenv ("GIO_USE_VFS", "local", TRUE);
  /* There's not really a "root dconf" right now; otherwise we might
   * try to spawn one via GSocketClient → GProxyResolver → GSettings.
   *
   * https://github.com/projectatomic/rpm-ostree/pull/312
   * https://bugzilla.gnome.org/show_bug.cgi?id=767183
   **/
  if (getuid () == 0)
    g_assert (g_setenv ("GSETTINGS_BACKEND", "memory", TRUE));
  g_set_prgname (argv[0]);

  setlocale (LC_ALL, "");

  /*
   * Parse the global options. We rearrange the options as
   * necessary, in order to pass relevant options through
   * to the commands, but also have them take effect globally.
   */
  command_name = rpmostree_subcommand_parse (&argc, argv, NULL);

  command = lookup_command (command_name);

  if (!command)
    {
      g_autoptr(GOptionContext) context =
        option_context_new_with_commands (NULL, commands);
      g_autofree char *help = NULL;

      /* This will not return for some options (e.g. --version). */
      (void) rpmostree_option_context_parse (context, NULL, &argc, &argv,
                                             NULL, NULL, NULL, NULL, NULL,
                                             NULL, NULL);
      if (command_name == NULL)
        {
          local_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "No command specified");
        }
      else
        {
          local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "Unknown command '%s'", command_name);
        }

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);
      exit_status = EXIT_FAILURE;

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), command_name);
  g_set_prgname (prgname);

  { RpmOstreeCommandInvocation invocation = { .command = command };
    exit_status = command->fn (argc, argv, &invocation, cancellable, &local_error);
  }

 out:
  if (local_error != NULL)
    {
      int is_tty = isatty (1);
      const char *prefix = "";
      const char *suffix = "";
      if (is_tty)
        {
          prefix = "\x1b[31m\x1b[1m"; /* red, bold */
          suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
        }
      g_dbus_error_strip_remote_error (local_error);
      g_printerr ("%serror: %s%s\n", prefix, suffix, local_error->message);
      g_error_free (local_error);

      /* Print a warning if the exit status indicates success when we
       * actually had an error, so it gets reported and fixed quickly. */
      g_warn_if_fail (exit_status != EXIT_SUCCESS);
    }

  rpmostree_polkit_agent_close ();

  return exit_status;
}
