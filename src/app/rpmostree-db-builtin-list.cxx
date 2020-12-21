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
_builtin_db_list (OstreeRepo      *repo,
                  GPtrArray       *revs,
                  const GPtrArray *patterns,
                  GCancellable    *cancellable,
                  GError         **error)
{
  for (guint num = 0; num < revs->len; num++)
    {
      auto rev = static_cast<const char *>(revs->pdata[num]);

      g_autofree char *checksum = NULL;
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &checksum, error))
        return FALSE;

      if (!g_str_equal (rev, checksum))
        printf ("ostree commit: %s (%s)\n", rev, checksum);
      else
        printf ("ostree commit: %s\n", rev);

      /* in the common case where no patterns are provided, use the smarter db_query API */
      if (!patterns)
        {
          g_autoptr(GPtrArray) packages =
            rpm_ostree_db_query_all (repo, checksum, cancellable, error);
          if (!packages)
            return FALSE;

          for (guint i = 0; i < packages->len; i++)
            {
              auto package = static_cast<RpmOstreePackage *>(g_ptr_array_index (packages, i));
              g_print (" %s\n", rpm_ostree_package_get_nevra (package));
            }
        }
      else
        {
          g_autoptr(RpmRevisionData) rpmrev = NULL;
          rpmrev = rpmrev_new (repo, checksum, patterns, cancellable, error);
          if (!rpmrev)
            return FALSE;

          rpmhdrs_list (rpmrev_get_headers (rpmrev));
        }
    }

  return TRUE;
}

gboolean
rpmostree_db_builtin_list (int argc, char **argv,
                           RpmOstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context =
    g_option_context_new ("REV... [PREFIX-PKGNAME...]");

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
