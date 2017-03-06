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

static RpmOstreeCommand supported_commands[] = {
#ifdef HAVE_COMPOSE_TOOLING
  { "compose", rpmostree_builtin_compose },
#endif
  { "cleanup", rpmostree_builtin_cleanup },
  { "db", rpmostree_builtin_db },
  { "deploy", rpmostree_builtin_deploy },
  { "rebase", rpmostree_builtin_rebase },
  { "rollback", rpmostree_builtin_rollback },
  { "status", rpmostree_builtin_status },
  { "upgrade", rpmostree_builtin_upgrade },
  { "reload", rpmostree_builtin_reload },
  { "initramfs", rpmostree_builtin_initramfs },
  { "install", rpmostree_builtin_pkg_add },
  { "uninstall", rpmostree_builtin_pkg_remove },
  { NULL }
};

static RpmOstreeCommand preview_commands[] = {
  { NULL }
};

static RpmOstreeCommand legacy_alias_commands[] = {
  { "pkg-add", rpmostree_builtin_pkg_add },
  { "pkg-remove", rpmostree_builtin_pkg_remove },
  { NULL }
};

static RpmOstreeCommand experimental_commands[] = {
  { "internals", rpmostree_builtin_internals },
  { "container", rpmostree_builtin_container },
  { NULL }
};

static RpmOstreeCommand hidden_commands[] = {
  { "start-daemon", rpmostree_builtin_start_daemon },
  { NULL }
};

static gboolean opt_version;
static gboolean opt_force_peer;
static char *opt_sysroot;

static GOptionEntry global_entries[] = {
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { NULL }
};

static GOptionEntry daemon_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "peer", 0, 0, G_OPTION_ARG_NONE, &opt_force_peer, "Force a peer-to-peer connection instead of using the system message bus", NULL },
  { NULL }
};

static GOptionContext *
option_context_new_with_commands (void)
{
  RpmOstreeCommand *command = supported_commands;
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new ("COMMAND");

  summary = g_string_new ("Builtin Commands:");

  while (command->name != NULL)
    {
      g_string_append_printf (summary, "\n  %s", command->name);
      command++;
    }

  command = preview_commands;
  while (command->name != NULL)
    {
      g_string_append_printf (summary, "\n  %s (preview)", command->name);
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
                                RpmOstreeBuiltinFlags flags,
                                GCancellable *cancellable,
                                RPMOSTreeSysroot **out_sysroot_proxy,
                                GError **error)
{
  gboolean use_daemon;
  gboolean ret = FALSE;

  use_daemon = ((flags & RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD) == 0);

  if (main_entries != NULL)
    g_option_context_add_main_entries (context, main_entries, NULL);

  if (use_daemon)
    g_option_context_add_main_entries (context, daemon_entries, NULL);

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!g_option_context_parse (context, argc, argv, error))
    goto out;

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
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "This command requires root privileges");
      goto out;
    }

  if (use_daemon)
    {
      if (!rpmostree_load_sysroot (opt_sysroot,
                                   opt_force_peer,
                                   cancellable,
                                   out_sysroot_proxy,
                                   error))
        goto out;
    }

  ret = TRUE;

out:
  return ret;
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
lookup_command_of_type (RpmOstreeCommand *commands,
                        const char *name,
                        const char *type)
{
  RpmOstreeCommand *command = commands;
  const int is_tty = isatty (1);
  const char *bold_prefix;
  const char *bold_suffix;

  bold_prefix = is_tty ? "\x1b[1m" : "";  
  bold_suffix = is_tty ? "\x1b[0m" : "";

  while (command->name)
    {
      if (g_strcmp0 (name, command->name) == 0)
        {
          if (type)
            g_printerr ("%snotice%s: %s is %s command and subject to change.\n",
                        bold_prefix, bold_suffix, name, type);
          return command;
        }
      command++;
    }
  return NULL;
}

int
main (int    argc,
      char **argv)
{
  GCancellable *cancellable = g_cancellable_new ();
  RpmOstreeCommand *command;
  int exit_status = EXIT_SUCCESS;
  int in, out;
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
  for (in = 1, out = 1; in < argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[in][0] != '-')
        {
          if (command_name == NULL)
            {
              command_name = argv[in];
              out--;
              continue;
            }
        }

      else if (g_str_equal (argv[in], "--"))
        {
          break;
        }

      argv[out] = argv[in];
    }

  argc = out;

  /* Keep the "rpm" command working for backward-compatibility. */
  if (g_strcmp0 (command_name, "rpm") == 0)
    command_name = "db";

  command = lookup_command_of_type (supported_commands, command_name, NULL);
  if (!command)
    command = lookup_command_of_type (legacy_alias_commands, command_name, NULL);

  if (!command)
    command = lookup_command_of_type (preview_commands, command_name, "a preview");

  if (!command)
    command = lookup_command_of_type (experimental_commands, command_name, "an experimental");

  if (!command)
    command = lookup_command_of_type (hidden_commands, command_name, NULL);

  if (!command)
    {
      g_autoptr(GOptionContext) context = option_context_new_with_commands ();
      g_autofree char *help = NULL;

      /* This will not return for some options (e.g. --version). */
      (void) rpmostree_option_context_parse (context, NULL, &argc, &argv,
                                             RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                             NULL, NULL, NULL);
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

  exit_status = command->fn (argc, argv, cancellable, &local_error);

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
