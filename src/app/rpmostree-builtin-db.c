/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 James Antil <james@fedoraproject.org>
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

#include "rpmostree-db-builtins.h"
#include "rpmostree-rpm-util.h"

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, GCancellable *cancellable, GError **error);
} RpmOstreeDbCommand;

static RpmOstreeDbCommand rpm_subcommands[] = {
  { "diff", rpmostree_db_builtin_diff },
  { "list", rpmostree_db_builtin_list },
  { "version", rpmostree_db_builtin_version },
  { NULL, NULL }
};

static char *opt_repo;

static GOptionEntry global_entries[] = {
  { "repo", 'r', 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository (defaults to /sysroot/ostree/repo)", "PATH" },
  { NULL }
};

static GOptionContext *
rpm_option_context_new_with_commands (void)
{
  RpmOstreeDbCommand *command = rpm_subcommands;
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new ("COMMAND");

  summary = g_string_new ("Builtin \"db\" Commands:");

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
rpmostree_db_option_context_parse (GOptionContext *context,
                                   const GOptionEntry *main_entries,
                                   int *argc, char ***argv,
                                   OstreeRepo **out_repo,
                                   GCancellable *cancellable, GError **error)
{
  gs_unref_object OstreeRepo *repo = NULL;
  gboolean success = FALSE;

  /* Entries are listed in --help output in the order added.  We add the
   * main entries ourselves so that we can add the --repo entry first. */

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       main_entries,
                                       argc, argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (opt_repo == NULL)
    {
      gs_unref_object OstreeSysroot *sysroot = NULL;

      sysroot = ostree_sysroot_new_default ();
      if (!ostree_sysroot_load (sysroot, cancellable, error))
        goto out;

      if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
        goto out;
    }
  else
    {
      gs_unref_object GFile *repo_file = g_file_new_for_path (opt_repo);

      repo = ostree_repo_new (repo_file);
      if (!ostree_repo_open (repo, cancellable, error))
        goto out;
    }

  if (rpmReadConfigFiles (NULL, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "rpm failed to init: %s", rpmlogMessage ());
      goto out;
    }

  gs_transfer_out_value (out_repo, &repo);

  success = TRUE;
out:
  return success;
}

gboolean
rpmostree_builtin_db (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  RpmOstreeDbCommand *subcommand;
  const char *subcommand_name = NULL;
  gs_free char *prgname = NULL;
  gboolean ret = FALSE;
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

  subcommand = rpm_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      GOptionContext *context;
      gs_free char *help;

      context = rpm_option_context_new_with_commands ();

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
                                   "No \"db\" subcommand specified");
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown \"db\" subcommand '%s'", subcommand_name);
            }
        }

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      g_option_context_free (context);

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), subcommand_name);
  g_set_prgname (prgname);

  if (!subcommand->fn (argc, argv, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

