/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "rpmostree-ex-builtins.h"
#include "rpmostree-container-builtins.h"
#include "rpmostree-rpm-util.h"

static RpmOstreeCommand ex_subcommands[] = {
  { "livefs", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    rpmostree_ex_builtin_livefs },
  { "unpack", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    rpmostree_ex_builtin_unpack },
  { "container", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    rpmostree_builtin_container },
  { NULL, 0, NULL }
};

/*
static GOptionEntry global_entries[] = {
  { NULL }
};
*/

static GOptionContext *
ex_option_context_new_with_commands (void)
{
  RpmOstreeCommand *command = ex_subcommands;
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new ("COMMAND");

  summary = g_string_new ("Builtin \"ex\" Commands:");

  while (command->name != NULL)
    {
      g_string_append_printf (summary, "\n  %s", command->name);
      command++;
    }

  g_option_context_set_summary (context, summary->str);

  g_string_free (summary, TRUE);

  return context;
}

int
rpmostree_builtin_ex (int argc, char **argv,
                      RpmOstreeCommandInvocation *invocation,
                      GCancellable *cancellable, GError **error)
{
  RpmOstreeCommand *subcommand;
  const char *subcommand_name = NULL;
  g_autofree char *prgname = NULL;
  int exit_status = EXIT_SUCCESS;

  subcommand_name = rpmostree_subcommand_parse (&argc, argv, invocation);
  subcommand = ex_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      g_autoptr(GOptionContext) context = ex_option_context_new_with_commands ();
      g_autofree char *help = NULL;

      (void) rpmostree_option_context_parse (context, NULL,
                                             &argc, &argv,
                                             invocation,
                                             cancellable,
                                             NULL, NULL, NULL, NULL, NULL);

      if (subcommand_name == NULL)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "No \"ex\" subcommand specified");
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unknown \"ex\" subcommand '%s'", subcommand_name);
        }

      exit_status = EXIT_FAILURE;
      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      goto out;
    }


  prgname = g_strdup_printf ("%s %s", g_get_prgname (), subcommand_name);
  g_set_prgname (prgname);

  { const int is_tty = isatty (1);
    const char *bold_prefix = is_tty ? "\x1b[1m" : "";
    const char *bold_suffix = is_tty ? "\x1b[0m" : "";

    g_printerr ("%snotice%s: \"%s\" is an experimental command and subject to change.\n",
                bold_prefix, bold_suffix, subcommand->name);

    RpmOstreeCommandInvocation sub_invocation = { .command = subcommand };
    exit_status = subcommand->fn (argc, argv, &sub_invocation, cancellable, error);
  }

 out:
  return exit_status;
}

