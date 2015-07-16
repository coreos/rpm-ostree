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

static GOptionEntry db_version_entries[] = {
  { NULL }
};

static gboolean
_builtin_db_version (OstreeRepo *repo, GPtrArray *revs,
                     GCancellable   *cancellable,
                     GError        **error)
{
  int num = 0;
  gboolean ret = FALSE;

  for (num = 0; num < revs->len; num++)
    {
      char *rev = revs->pdata[num];
      _cleanup_rpmrev_ struct RpmRevisionData *rpmrev = NULL;
      gs_free char *rpmdbv = NULL;
      char *mrev = strstr (rev, "..");

      if (mrev)
        {
          gs_unref_ptrarray GPtrArray *range_revs = NULL;
          gs_free char *revdup = g_strdup (rev);

          mrev = revdup + (mrev - rev);
          *mrev = 0;
          mrev += 2;

          if (!*mrev)
            mrev = NULL;

          range_revs = _rpmostree_util_get_commit_hashes (repo, revdup, mrev, cancellable, error);
          if (!range_revs)
            goto out;

          if (!_builtin_db_version (repo, range_revs,
                                    cancellable, error))
            goto out;

          continue;
        }

        rpmrev = rpmrev_new (repo, rev, NULL, cancellable, error);
        if (!rpmrev)
          goto out;

        rpmdbv = rpmhdrs_rpmdbv (rpmrev_get_headers (rpmrev),
                                 cancellable, error);
        if (rpmdbv == NULL)
          goto out;

        /* FIXME: g_console? */
        if (!g_str_equal (rev, rpmrev_get_commit (rpmrev)))
          printf ("ostree commit: %s (%s)\n", rev, rpmrev_get_commit (rpmrev));
        else
          printf ("ostree commit: %s\n", rev);

        printf ("  rpmdbv is: %66s\n", rpmdbv);
      }

  ret = TRUE;

 out:
  return ret;
}

gboolean
rpmostree_db_builtin_version (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_ptrarray GPtrArray *revs = NULL;
  gboolean success = FALSE;
  gint ii;

  context = g_option_context_new ("COMMIT... - Show rpmdb version of packages within the commits");

  if (!rpmostree_db_option_context_parse (context, db_version_entries, &argc, &argv, &repo,
                                          cancellable, error))
    goto out;

  revs = g_ptr_array_new ();

  for (ii = 1; ii < argc; ii++)
    g_ptr_array_add (revs, argv[ii]);

  if (!_builtin_db_version (repo, revs, cancellable, error))
    goto out;

  success = TRUE;

out:
  g_option_context_free (context);

  return success;
}

