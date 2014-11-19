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

#include "rpmostree-compose-builtins.h"
#include "rpmostree-builtins.h"

#include <ostree.h>
#include <libgsystem.h>

#include <glib/gi18n.h>

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, GCancellable *cancellable, GError **error);
} RpmOstreeComposeCommand;

static RpmOstreeComposeCommand compose_subcommands[] = {
  { "tree", rpmostree_compose_builtin_tree },
  { "sign", rpmostree_compose_builtin_sign },
  { NULL, NULL }
};

gboolean
rpmostree_builtin_compose (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  RpmOstreeComposeCommand *subcommand;
  const char *subcommand_name = NULL;
  int in, out, i;
  gboolean skip;

  for (in = 1, out = 1; in < argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[in][0] != '-')
        {
          skip = (subcommand_name == NULL);
          if (subcommand_name == NULL)
            subcommand_name = argv[in];
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
              /* If a subcommand was given, let GOptionContext handle the
               * help option.  Otherwise eat it and list the subcommands. */
            }
          else if (subcommand_name == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown or invalid compose option: %s", argv[in]);
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
                  /* If a subcommand was given, let GOptionContext handle the
                   * help option.  Otherwise eat it and list the subcommands. */
                  break;

                default:
                  if (subcommand_name == NULL)
                    {
                      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Unknown or invalid compose option: %s", argv[in]);
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

  if (subcommand_name == NULL)
    {
      subcommand = compose_subcommands;
      g_print ("usage: %s COMMAND [options]\n", g_get_prgname ());
      g_print ("Builtin commands:\n");
      while (subcommand->name)
        {
          g_print ("  %s\n", subcommand->name);
          subcommand++;
        }
      return 1;
    }

  subcommand = compose_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unknown compose command '%s'", subcommand_name);
      goto out;
    }

  if (!subcommand->fn (argc, argv, cancellable, error))
    goto out;
 
  ret = TRUE;
 out:
  return ret;
}
