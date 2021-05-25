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

static RpmOstreeCommand rpm_subcommands[] = {
  { "diff", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Show package changes between two commits",
    rpmostree_db_builtin_diff },
  { "list", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "List packages within commits",
    rpmostree_db_builtin_list },
  { "version", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Show rpmdb version of packages within the commits",
    rpmostree_db_builtin_version },
  { NULL, (RpmOstreeBuiltinFlags)0, NULL, NULL }
};

static char *opt_repo;

static GOptionEntry global_entries[] = {
  { "repo", 'r', 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository (defaults to /sysroot/ostree/repo)", "PATH" },
  { NULL }
};

gboolean
rpmostree_db_option_context_parse (GOptionContext *context,
                                   const GOptionEntry *main_entries,
                                   int *argc, char ***argv,
                                   RpmOstreeCommandInvocation *invocation,
                                   OstreeRepo **out_repo,
                                   GCancellable *cancellable, GError **error)
{
  /* Entries are listed in --help output in the order added.  We add the
   * main entries ourselves so that we can add the --repo entry first. */

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       main_entries,
                                       argc, argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL,
                                       error))
    return FALSE;

  g_autoptr(OstreeRepo) repo = NULL;
  if (opt_repo == NULL)
    {
      g_autoptr(OstreeSysroot) sysroot = ostree_sysroot_new_default ();

      if (!ostree_sysroot_load (sysroot, cancellable, error))
        return FALSE;

      if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
        return FALSE;
    }
  else
    {
      repo = ostree_repo_open_at (AT_FDCWD, opt_repo, cancellable, error);
      if (!repo)
        return FALSE;
    }

  if (rpmReadConfigFiles (NULL, NULL))
    return glnx_throw (error, "rpm failed to init: %s", rpmlogMessage ());

  *out_repo = util::move_nullify (repo);
  return TRUE;
}

gboolean
rpmostree_builtin_db (int argc, char **argv,
                      RpmOstreeCommandInvocation *invocation,
                      GCancellable *cancellable, GError **error)
{
  return rpmostree_handle_subcommand (argc, argv, rpm_subcommands,
                                      invocation, cancellable, error);
}

