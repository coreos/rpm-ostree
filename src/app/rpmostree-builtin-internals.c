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

#include "rpmostree-internals-builtins.h"
#include "rpmostree-rpm-util.h"

typedef struct {
  const char *name;
  int (*fn) (int argc, char **argv, GCancellable *cancellable, GError **error);
} RpmOstreeInternalsCommand;

static RpmOstreeInternalsCommand internals_subcommands[] = {
  { "unpack", rpmostree_internals_builtin_unpack },
  { NULL, NULL }
};

/*
static GOptionEntry global_entries[] = {
  { NULL }
};
*/

static GOptionContext *
internals_option_context_new_with_commands (void)
{
  RpmOstreeInternalsCommand *command = internals_subcommands;
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new ("COMMAND");

  summary = g_string_new ("Builtin \"internals\" Commands:");

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
rpmostree_builtin_internals (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  RpmOstreeInternalsCommand *subcommand;
  const char *subcommand_name = NULL;
  gs_free char *prgname = NULL;
  int exit_status = EXIT_SUCCESS;
  int in, out;

  for (in = 1, out = 1; in < argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[in][0] != '-')
        {
          if (subcommand_name == NULL)
            {
              subcommand_name = argv[in];
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

  subcommand = internals_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      GOptionContext *context;
      gs_free char *help = NULL;

      context = internals_option_context_new_with_commands ();

      /* This will not return for some options (e.g. --version). */
      if (rpmostree_option_context_parse (context, NULL,
                                          &argc, &argv,
                                          RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                          cancellable,
                                          NULL,
                                          error))
        {
          if (subcommand_name == NULL)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No \"internals\" subcommand specified");
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown \"internals\" subcommand '%s'", subcommand_name);
            }
          exit_status = EXIT_FAILURE;
        }

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      g_option_context_free (context);

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), subcommand_name);
  g_set_prgname (prgname);

  exit_status = subcommand->fn (argc, argv, cancellable, error);

 out:
  return exit_status;
}

