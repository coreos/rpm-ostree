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
_builtin_db_list (OstreeRepo *repo, GFile *rpmdbdir,
                  GPtrArray *revs, const GPtrArray *patterns,
                  GCancellable   *cancellable,
                  GError        **error)
{
  int num = 0;
  gboolean ret = FALSE;

  for (num = 0; num < revs->len; num++)
    {
      char *rev = revs->pdata[num];
      _cleanup_rpmrev_ struct RpmRevisionData *rpmrev = NULL;
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

          if (!_builtin_db_list (repo, rpmdbdir, range_revs, patterns,
                                 cancellable, error))
            goto out;

          continue;
        }

      rpmrev = rpmrev_new (repo, rpmdbdir, rev, patterns,
                           cancellable, error);
      if (!rpmrev)
        goto out;

      if (!g_str_equal (rev, rpmrev->commit))
        printf ("ostree commit: %s (%s)\n", rev, rpmrev->commit);
      else
        printf ("ostree commit: %s\n", rev);

      rpmhdrs_list (rpmrev->root, rpmrev->rpmdb);
    }

  ret = TRUE;

 out:
  return ret;
}

gboolean
rpmostree_db_builtin_list (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object GFile *rpmdbdir = NULL;
  gboolean rpmdbdir_is_tmp = FALSE;
  gs_unref_ptrarray GPtrArray *patterns = NULL;
  gs_unref_ptrarray GPtrArray *revs = NULL;
  GQueue queue = G_QUEUE_INIT;
  gboolean success = FALSE;
  int ii;

  context = g_option_context_new ("[PREFIX-PKGNAME...] COMMIT... - List packages within commits");

  if (!rpmostree_db_option_context_parse (context, option_entries, &argc, &argv, &repo,
                                          &rpmdbdir, &rpmdbdir_is_tmp, cancellable, error))
    goto out;

  /* Put all the arguments in a GPtrArray (because it's easier to deal with
   * than a string vector), and then split the trailing arguments which are
   * valid commit checksums into a separate GPtrArray. */

  patterns = g_ptr_array_new ();
  revs = g_ptr_array_new ();

  for (ii = 1; ii < argc; ii++)
    g_ptr_array_add (patterns, argv[ii]);

    while (patterns->len > 0)
      {
        gs_free char *commit = NULL;
        guint index = patterns->len - 1;
        char *arg = patterns->pdata[index];

        ostree_repo_resolve_rev (repo, arg, TRUE, &commit, NULL);

        /* Stop when we find a non-commit argument. */
        if (commit == NULL)
          break;

        /* XXX Need to PREPEND to the 'revs' array to preserve order, but
         *     in GLib versions prior to 2.40 you can only (easily) APPEND
         *     to a GPtrArray.  g_ptr_array_insert() is not available to us
         *     at this time, so use a GQueue to swap the order. */
        g_queue_push_head (&queue, arg);
        g_ptr_array_remove_index (patterns, index);
      }

    while (!g_queue_is_empty (&queue))
      g_ptr_array_add (revs, g_queue_pop_head (&queue));

    if (!_builtin_db_list (repo, rpmdbdir, revs, patterns, cancellable, error))
      goto out;

  success = TRUE;

out:
  if (rpmdbdir_is_tmp)
    (void) gs_shutil_rm_rf (rpmdbdir, NULL, NULL);

  g_option_context_free (context);

  return success;
}

