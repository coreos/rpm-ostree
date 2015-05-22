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

static char *opt_format;

static GOptionEntry option_entries[] = {
  { "format", 'F', 0, G_OPTION_ARG_STRING, &opt_format, "Output format: \"diff\" or (default) \"block\"", "FORMAT" },
  { NULL }
};

gboolean
rpmostree_db_builtin_diff (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object GFile *rpmdbdir = NULL;
  gboolean rpmdbdir_is_tmp = FALSE;
  struct RpmRevisionData *rpmrev1 = NULL;
  struct RpmRevisionData *rpmrev2 = NULL;
  gboolean success = FALSE;

  context = g_option_context_new ("COMMIT COMMIT - Show package changes between two commits");

  if (!rpmostree_db_option_context_parse (context, option_entries, &argc, &argv, &repo,
                                          &rpmdbdir, &rpmdbdir_is_tmp, cancellable, error))
    goto out;

  if (argc != 3)
    {
      gs_free char *help;

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "\"%s\" takes exactly 2 arguments", g_get_prgname ());

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      goto out;
    }

  if (!(rpmrev1 = rpmrev_new (repo, rpmdbdir, argv[1], NULL, cancellable, error)))
    goto out;

  if (!(rpmrev2 = rpmrev_new (repo, rpmdbdir, argv[2], NULL, cancellable, error)))
    goto out;

  if (!g_str_equal (argv[1], rpmrev1->commit))
    printf ("ostree diff commit old: %s (%s)\n", argv[1], rpmrev1->commit);
  else
    printf ("ostree diff commit old: %s\n", argv[1]);

  if (!g_str_equal (argv[2], rpmrev2->commit))
    printf ("ostree diff commit new: %s (%s)\n", argv[2], rpmrev2->commit);
  else
    printf ("ostree diff commit new: %s\n", argv[2]);

  if (opt_format == NULL)
    opt_format = "block";

  if (g_str_equal (opt_format, "diff"))
    {
      rpmhdrs_diff_prnt_diff (rpmhdrs_diff (rpmrev1->rpmdb, rpmrev2->rpmdb));
    }
  else if (g_str_equal (opt_format, "block"))
    {
      rpmhdrs_diff_prnt_block (rpmhdrs_diff (rpmrev1->rpmdb, rpmrev2->rpmdb));
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Format argument is invalid, pick one of: diff, block");
      goto out;
    }

  success = TRUE;

out:
  /* Free the RpmRevisionData structs explicitly *before* possibly removing
   * the database directory, since rpmhdrs_free() depends on that directory
   * being there. */
  rpmrev_free (rpmrev1);
  rpmrev_free (rpmrev2);

  if (rpmdbdir_is_tmp)
    (void) gs_shutil_rm_rf (rpmdbdir, NULL, NULL);

  g_option_context_free (context);

  return success;
}

