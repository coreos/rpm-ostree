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

#include "rpmostree.h"
#include "rpmostree-db-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"

static char *opt_format = "block";
static gboolean opt_changelogs;

static GOptionEntry option_entries[] = {
  { "format", 'F', 0, G_OPTION_ARG_STRING, &opt_format, "Output format: \"diff\" or (default) \"block\"", "FORMAT" },
  { "changelogs", 'c', 0, G_OPTION_ARG_NONE, &opt_changelogs, "Also output RPM changelogs", NULL },
  { NULL }
};

int
rpmostree_db_builtin_diff (int argc, char **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("COMMIT COMMIT");

  g_autoptr(OstreeRepo) repo = NULL;
  if (!rpmostree_db_option_context_parse (context, option_entries, &argc, &argv, invocation,
                                          &repo, cancellable, error))
    return EXIT_FAILURE;

  if (argc != 3)
    {
      g_autofree char *message =
        g_strdup_printf ("\"%s\" takes exactly 2 arguments", g_get_prgname ());
      rpmostree_usage_error (context, message, error);
      return EXIT_FAILURE;
    }

  const char *old_ref = argv[1];
  g_autofree char *old_checksum = NULL;
  if (!ostree_repo_resolve_rev (repo, old_ref, FALSE, &old_checksum, error))
    return EXIT_FAILURE;

  const char *new_ref = argv[2];
  g_autofree char *new_checksum = NULL;
  if (!ostree_repo_resolve_rev (repo, new_ref, FALSE, &new_checksum, error))
    return EXIT_FAILURE;

  if (!g_str_equal (old_ref, old_checksum))
    printf ("ostree diff commit old: %s (%s)\n", old_ref, old_checksum);
  else
    printf ("ostree diff commit old: %s\n", old_ref);

  if (!g_str_equal (new_ref, new_checksum))
    printf ("ostree diff commit new: %s (%s)\n", new_ref, new_checksum);
  else
    printf ("ostree diff commit new: %s\n", new_ref);

  g_autoptr(GPtrArray) removed = NULL;
  g_autoptr(GPtrArray) added = NULL;
  g_autoptr(GPtrArray) modified_old = NULL;
  g_autoptr(GPtrArray) modified_new = NULL;

  /* we still use the old API for changelogs; should enhance libdnf for this */
  if (g_str_equal (opt_format, "block") && opt_changelogs)
    {
      g_autoptr(RpmRevisionData) rpmrev1 = rpmrev_new (repo, old_ref, NULL, cancellable, error);
      if (!rpmrev1)
        return EXIT_FAILURE;
      g_autoptr(RpmRevisionData) rpmrev2 = rpmrev_new (repo, new_ref, NULL, cancellable, error);
      if (!rpmrev2)
        return EXIT_FAILURE;

      rpmhdrs_diff_prnt_block (TRUE, rpmhdrs_diff (rpmrev_get_headers (rpmrev1),
                                                   rpmrev_get_headers (rpmrev2)));
    }
  else
    {
      if (!rpm_ostree_db_diff (repo, old_ref, new_ref, &removed, &added, &modified_old,
                               &modified_new, cancellable, error))
        return EXIT_FAILURE;

      if (g_str_equal (opt_format, "diff"))
        rpmostree_diff_print (removed, added, modified_old, modified_new);
      else if (g_str_equal (opt_format, "block"))
        rpmostree_diff_print_formatted (removed, added, modified_old, modified_new);
      else
        {
          glnx_throw (error, "Format argument is invalid, pick one of: diff, block");
          return EXIT_FAILURE;
        }
    }

  return EXIT_SUCCESS;
}

