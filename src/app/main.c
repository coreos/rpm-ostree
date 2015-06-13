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

#include "libgsystem.h"

static RpmOstreeCommand commands[] = {
#ifdef HAVE_COMPOSE_TOOLING
  { "compose", rpmostree_builtin_compose },
#endif
  { "db", rpmostree_builtin_db },
  { "rebase", rpmostree_builtin_rebase },
  { "rollback", rpmostree_builtin_rollback },
  { "status", rpmostree_builtin_status },
  { "upgrade", rpmostree_builtin_upgrade },
  { NULL }
};

static gboolean opt_version;

static GOptionEntry global_entries[] = {
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
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
                                GError **error)
{
  if (main_entries != NULL)
    g_option_context_add_main_entries (context, main_entries, NULL);

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!g_option_context_parse (context, argc, argv, error))
    return FALSE;

  if (opt_version)
    {
      g_print ("%s\n  %s\n", PACKAGE_NAME, RPM_OSTREE_FEATURES);
      exit (EXIT_SUCCESS);
    }

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


static gboolean
on_sigint (gpointer user_data)
{
  GCancellable *cancellable = user_data;
  g_debug ("Caught signal. Canceling");
  g_cancellable_cancel (cancellable);
  return FALSE;
}


int
main (int    argc,
      char **argv)
{
  GError *error = NULL;
  GCancellable *cancellable = g_cancellable_new ();
  RpmOstreeCommand *command;
  int in, out;
  const char *command_name = NULL;
  gs_free char *prgname = NULL;

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

  g_unix_signal_add (SIGINT, on_sigint, cancellable);
  g_unix_signal_add (SIGTERM, on_sigint, cancellable);
  g_unix_signal_add (SIGHUP, on_sigint, cancellable);

  /* Keep the "rpm" command working for backward-compatibility. */
  if (g_strcmp0 (command_name, "rpm") == 0)
    command_name = "db";

  command = commands;
  while (command->name)
    {
      if (g_strcmp0 (command_name, command->name) == 0)
        break;
      command++;
    }

  if (!command->fn)
    {
      GOptionContext *context;
      gs_free char *help;

      context = option_context_new_with_commands ();

      /* This will not return for some options (e.g. --version). */
      if (rpmostree_option_context_parse (context, NULL, &argc, &argv, &error))
        {
          if (command_name == NULL)
            {
              g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No command specified");
            }
          else
            {
              g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown command '%s'", command_name);
            }
        }

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      g_option_context_free (context);

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), command_name);
  g_set_prgname (prgname);

  if (!command->fn (argc, argv, cancellable, &error))
    goto out;

 out:
  if (error != NULL)
    {
      int is_tty = isatty (1);
      const char *prefix = "";
      const char *suffix = "";
      if (is_tty)
        {
          prefix = "\x1b[31m\x1b[1m"; /* red, bold */
          suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
        }
      g_dbus_error_strip_remote_error (error);
      g_printerr ("%serror: %s%s\n", prefix, suffix, error->message);
      g_error_free (error);
      return 1;
    }
  return 0;
}
