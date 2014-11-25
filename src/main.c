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

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>

#include "rpmostree-builtins.h"

#include "libgsystem.h"

static RpmOstreeCommand commands[] = {
#ifdef HAVE_COMPOSE_TOOLING
  { "compose", rpmostree_builtin_compose, 0 },
#endif
  { "upgrade", rpmostree_builtin_upgrade, 0 },
  { "rebase", rpmostree_builtin_rebase, 0 },
  { "rollback", rpmostree_builtin_rollback, 0 },
  { "status", rpmostree_builtin_status, 0 },
  { "rpm", rpmostree_builtin_rpm, 0 },
  { NULL }
};

static int
usage (char    **argv,
       gboolean is_error)
{
  RpmOstreeCommand *command = commands;
  void (*print_func) (const gchar *format, ...);

  if (is_error)
    print_func = g_printerr;
  else
    print_func = g_print;

  print_func ("usage: %s COMMAND [options]\n",
              argv[0]);
  print_func ("Builtin commands:\n");

  while (command->name)
    {
      print_func ("  %s\n", command->name);
      command++;
    }
  return (is_error ? 1 : 0);
}

int
main (int    argc,
      char **argv)
{
  GError *error = NULL;
  GCancellable *cancellable = NULL;
  RpmOstreeCommand *command;
  int i, in, out;
  gboolean skip;
  gboolean want_help = FALSE;
  const char *cmd = NULL;
  
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
          skip = (cmd == NULL);
          if (cmd == NULL)
              cmd = argv[in];
        }

      /* The global long options */
      else if (argv[in][1] == '-')
        {
          skip = FALSE;

          if (g_str_equal (argv[in], "--"))
            {
              break;
            }
          else if (g_str_equal (argv[in], "--help"))
            {
              want_help = TRUE;
            }
          else if (cmd == NULL && g_str_equal (argv[in], "--version"))
            {
              g_print ("%s\n  %s\n", PACKAGE_STRING, RPM_OSTREE_FEATURES);
              return 0;
            }
          else if (cmd == NULL)
            {
              g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown or invalid global option: %s", argv[in]);
              goto out;
            }
        }

      /* The global short options */
      else
        {
          skip = FALSE;
          for (i = 1; argv[in][i] != '\0'; i++)
            {
              switch (argv[in][i])
              {
                case 'h':
                  want_help = TRUE;
                  break;
                default:
                  if (cmd == NULL)
                    {
                      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Unknown or invalid global option: %s", argv[in]);
                      goto out;
                    }
                  break;
              }
            }
        }

      /* Skipping this argument? */
      if (skip)
        out--;
      else
        argv[out] = argv[in];
    }

  argc = out;

  if (cmd == NULL)
    {
      if (!want_help)
        {
          g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "No command specified");
        }
      usage (argv, TRUE);
      goto out;
    }

  command = commands;
  while (command->name)
    {
      if (g_strcmp0 (cmd, command->name) == 0)
        break;
      command++;
    }

  if (!command->fn)
    {
      gs_free char *msg = g_strdup_printf ("Unknown command '%s'", cmd);
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, msg);
      goto out;
    }

  g_set_prgname (g_strdup_printf (PACKAGE_STRING " %s", cmd));

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
      g_printerr ("%serror: %s%s\n", prefix, suffix, error->message);
      g_error_free (error);
      return 1;
    }
  return 0;
}
