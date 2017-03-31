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

#include "libglnx.h"

static RpmOstreeCommand commands[] = {
#ifdef HAVE_COMPOSE_TOOLING
  { "compose", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD |
               RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    rpmostree_builtin_compose },
#endif
  { "cleanup", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    rpmostree_builtin_cleanup },
  { "db", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    rpmostree_builtin_db },
  { "deploy", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT |
              RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS,
    rpmostree_builtin_deploy },
  { "rebase", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT |
              RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS,
    rpmostree_builtin_rebase },
  { "rollback", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    rpmostree_builtin_rollback },
  { "status", 0,
    rpmostree_builtin_status },
  { "upgrade", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT |
               RPM_OSTREE_BUILTIN_FLAG_SUPPORTS_PKG_INSTALLS,
    rpmostree_builtin_upgrade },
  { "reload", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    rpmostree_builtin_reload },
  { "initramfs", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    rpmostree_builtin_initramfs },
  { "install", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    rpmostree_builtin_pkg_add },
  { "uninstall", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    rpmostree_builtin_pkg_remove },
  /* Legacy aliases */
  { "pkg-add", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT | RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    rpmostree_builtin_pkg_add },
  { "pkg-remove", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT | RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    rpmostree_builtin_pkg_remove },
  { "ex", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD | RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    rpmostree_builtin_ex },
  /* Hidden */
  { "start-daemon", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD | RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT |
                   RPM_OSTREE_BUILTIN_FLAG_HIDDEN,
    rpmostree_builtin_start_daemon },
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
option_context_new_with_commands (void)
{
  RpmOstreeCommand *command = commands;
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new ("COMMAND");

  summary = g_string_new ("Builtin Commands:");

  while (command->name != NULL)
    {
      gboolean is_hidden = (command->flags & RPM_OSTREE_BUILTIN_FLAG_HIDDEN) > 0;
      if (!is_hidden)
        g_string_append_printf (summary, "\n  %s", command->name);
      command++;
    }

  g_option_context_set_summary (context, summary->str);

  g_string_free (summary, TRUE);

  return context;
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

void
rpmostree_print_gpg_verify_result (OstreeGpgVerifyResult *result)
{
  GString *buffer;
  guint n_sigs, ii;

  n_sigs = ostree_gpg_verify_result_count_all (result);

  /* XXX If we ever add internationalization, use ngettext() here. */
  g_print ("GPG: Verification enabled, found %u signature%s:\n",
           n_sigs, n_sigs == 1 ? "" : "s");

  buffer = g_string_sized_new (256);

  for (ii = 0; ii < n_sigs; ii++)
    {
      g_string_append_c (buffer, '\n');
      ostree_gpg_verify_result_describe (result, ii, buffer, "  ",
                                         OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
    }

  g_print ("%s\n", buffer->str);
  g_string_free (buffer, TRUE);
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
  g_set_prgname (argv[0]);

  setlocale (LC_ALL, "");

  /*
   * Parse the global options. We rearrange the options as
   * necessary, in order to pass relevant options through
   * to the commands, but also have them take effect globally.
   */
  command_name = rpmostree_subcommand_parse (&argc, argv, NULL);

  /* Keep the "rpm" command working for backward-compatibility. */
  if (g_strcmp0 (command_name, "rpm") == 0)
    command_name = "db";

  command = lookup_command (command_name);

  if (!command)
    {
      g_autoptr(GOptionContext) context = option_context_new_with_commands ();
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

  return exit_status;
}
