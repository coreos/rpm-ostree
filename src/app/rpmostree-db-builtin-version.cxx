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

static GOptionEntry db_version_entries[] = { { NULL } };

static gboolean
_builtin_db_version (OstreeRepo *repo, GPtrArray *revs, GCancellable *cancellable, GError **error)
{
  for (guint num = 0; num < revs->len; num++)
    {
      auto rev = static_cast<const char *> (revs->pdata[num]);

      g_autoptr (RpmRevisionData) rpmrev = rpmrev_new (repo, rev, NULL, cancellable, error);
      if (!rpmrev)
        return FALSE;

      g_autofree char *rpmdbv = rpmhdrs_rpmdbv (rpmrev_get_headers (rpmrev), cancellable, error);
      if (rpmdbv == NULL)
        return FALSE;

      if (!g_str_equal (rev, rpmrev_get_commit (rpmrev)))
        printf ("ostree commit: %s (%s)\n", rev, rpmrev_get_commit (rpmrev));
      else
        printf ("ostree commit: %s\n", rev);

      printf ("  rpmdbv is: %66s\n", rpmdbv);
    }

  return TRUE;
}

gboolean
rpmostree_db_builtin_version (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                              GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("COMMIT...");

  g_autoptr (OstreeRepo) repo = NULL;
  if (!rpmostree_db_option_context_parse (context, db_version_entries, &argc, &argv, invocation,
                                          &repo, cancellable, error))
    return FALSE;

  g_autoptr (GPtrArray) revs = g_ptr_array_new ();

  for (int ii = 1; ii < argc; ii++)
    g_ptr_array_add (revs, argv[ii]);

  if (!_builtin_db_version (repo, revs, cancellable, error))
    return FALSE;

  return TRUE;
}
