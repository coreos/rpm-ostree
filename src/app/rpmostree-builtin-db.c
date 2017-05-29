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
    rpmostree_db_builtin_diff },
  { "list", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    rpmostree_db_builtin_list },
  { "version", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    rpmostree_db_builtin_version },
  { NULL, 0, NULL }
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
  glnx_unref_object OstreeRepo *repo = NULL;
  gboolean success = FALSE;

  /* Entries are listed in --help output in the order added.  We add the
   * main entries ourselves so that we can add the --repo entry first. */

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       main_entries,
                                       argc, argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL,
                                       error))
    goto out;

  if (opt_repo == NULL)
    {
      glnx_unref_object OstreeSysroot *sysroot = NULL;

      sysroot = ostree_sysroot_new_default ();
      if (!ostree_sysroot_load (sysroot, cancellable, error))
        goto out;

      if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
        goto out;
    }
  else
    {
      g_autoptr(GFile) repo_file = g_file_new_for_path (opt_repo);

      repo = ostree_repo_new (repo_file);
      if (!ostree_repo_open (repo, cancellable, error))
        goto out;
    }

  if (rpmReadConfigFiles (NULL, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "rpm failed to init: %s", rpmlogMessage ());
      goto out;
    }

  *out_repo = g_steal_pointer (&repo);

  success = TRUE;
out:
  return success;
}

int
rpmostree_builtin_db (int argc, char **argv,
                      RpmOstreeCommandInvocation *invocation,
                      GCancellable *cancellable, GError **error)
{
  return rpmostree_handle_subcommand (argc, argv, rpm_subcommands,
                                      invocation, cancellable, error);
}

