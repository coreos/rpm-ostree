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

static GOptionEntry option_entries[] = {
  { NULL }
};

static gboolean
_builtin_db_list (OstreeRepo *repo,
                  GPtrArray *revs, const GPtrArray *patterns,
                  GCancellable   *cancellable,
                  GError        **error)
{
  for (guint num = 0; num < revs->len; num++)
    {
      char *rev = revs->pdata[num];
      g_autoptr(RpmRevisionData) rpmrev = NULL;
      char *mrev = strstr (rev, "..");

      if (mrev)
        {
          g_autoptr(GPtrArray) range_revs = NULL;
          g_autofree char *revdup = g_strdup (rev);

          mrev = revdup + (mrev - rev);
          *mrev = 0;
          mrev += 2;

          if (!*mrev)
            mrev = NULL;

          range_revs = _rpmostree_util_get_commit_hashes (repo, revdup, mrev, cancellable, error);
          if (!range_revs)
            return FALSE;

          if (!_builtin_db_list (repo, range_revs, patterns,
                                 cancellable, error))
            return FALSE;

          continue;
        }

      rpmrev = rpmrev_new (repo, rev, patterns,
                           cancellable, error);
      if (!rpmrev)
        return FALSE;

      if (!g_str_equal (rev, rpmrev_get_commit (rpmrev)))
        printf ("ostree commit: %s (%s)\n", rev, rpmrev_get_commit (rpmrev));
      else
        printf ("ostree commit: %s\n", rev);

      rpmhdrs_list (rpmrev_get_headers (rpmrev));
    }

  return TRUE;
}

int
rpmostree_db_builtin_list (int argc, char **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context =
    g_option_context_new ("[PREFIX-PKGNAME...] COMMIT...");

  g_autoptr(OstreeRepo) repo = NULL;
  if (!rpmostree_db_option_context_parse (context, option_entries, &argc, &argv,
                                          invocation, &repo, cancellable, error))
    return FALSE;

  /* Iterate over all arguments. When we see the first argument which
   * appears to be an OSTree commit, take all other arguments to be
   * patterns.
   */
  g_autoptr(GPtrArray) revs = g_ptr_array_new ();
  g_autoptr(GPtrArray) patterns = NULL;

  for (int ii = 1; ii < argc; ii++)
    {
      if (patterns != NULL)
        g_ptr_array_add (patterns, argv[ii]);
      else
        {
          g_autofree char *commit = NULL;

          ostree_repo_resolve_rev (repo, argv[ii], TRUE, &commit, NULL);

          if (!commit)
            {
              patterns = g_ptr_array_new ();
              g_ptr_array_add (patterns, argv[ii]);
            }
          else
            g_ptr_array_add (revs, argv[ii]);
        }
    }

  if (!_builtin_db_list (repo, revs, patterns, cancellable, error))
    return FALSE;

  return TRUE;
}

