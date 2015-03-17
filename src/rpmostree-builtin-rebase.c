/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <string.h>
#include <glib-unix.h>

#include "rpmostree-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-treepkgdiff.h"

#include "libgsystem.h"

static char *opt_sysroot = "/";
static char *opt_osname;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { NULL }
};

gboolean
rpmostree_builtin_rebase (int             argc,
                          char          **argv,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("REFSPEC - Switch to a different tree");
  const char *new_provided_refspec;
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_free char *origin_refspec = NULL;
  gs_free char *origin_remote = NULL;
  gs_free char *origin_ref = NULL;
  gs_free char *new_remote = NULL;
  gs_free char *new_ref = NULL;
  gs_free char *new_refspec = NULL;
  gs_unref_object GFile *sysroot_path = NULL;
  gs_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  gs_unref_object OstreeAsyncProgress *progress = NULL;
  gboolean changed;
  GSConsole *console = NULL;
  gs_unref_keyfile GKeyFile *old_origin = NULL;
  gs_unref_keyfile GKeyFile *new_origin = NULL;
  
  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "REFSPEC must be specified");
      goto out;
    }

  new_provided_refspec = argv[1];

  sysroot_path = g_file_new_for_path (opt_sysroot);
  sysroot = ostree_sysroot_new (sysroot_path);
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  upgrader = ostree_sysroot_upgrader_new_for_os_with_flags (sysroot, opt_osname,
                                                            OSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED,
                                                            cancellable, error);
  if (!upgrader)
    goto out;

  old_origin = ostree_sysroot_upgrader_get_origin (upgrader);
  origin_refspec = g_key_file_get_string (old_origin, "origin", "refspec", NULL);
  
  if (!ostree_parse_refspec (origin_refspec, &origin_remote, &origin_ref, error))
    goto out;

  /* Allow just switching remotes */
  if (g_str_has_suffix (new_provided_refspec, ":"))
    {
      new_remote = g_strdup (new_provided_refspec);
      new_remote[strlen(new_remote)-1] = '\0';
      new_ref = g_strdup (origin_ref);
    }
  else
    {
      if (!ostree_parse_refspec (new_provided_refspec, &new_remote, &new_ref, error))
        goto out;
    }
  
  if (!new_remote)
    new_refspec = g_strconcat (origin_remote, ":", new_ref, NULL);
  else
    new_refspec = g_strconcat (new_remote, ":", new_ref, NULL);
  
  if (strcmp (origin_refspec, new_refspec) == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Old and new refs are equal: %s", new_refspec);
      goto out;
    }

  new_origin = ostree_sysroot_origin_new_from_refspec (sysroot, new_refspec);
  if (!ostree_sysroot_upgrader_set_origin (upgrader, new_origin, cancellable, error))
    goto out;

  console = gs_console_get ();
  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
    }

  /* Always allow older...there's not going to be a chronological
   * relationship necessarily.
   */
  if (!ostree_sysroot_upgrader_pull (upgrader, 0,
                                     OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER,
                                     progress, &changed,
                                     cancellable, error))
    goto out;

  if (console)
    {
      if (!gs_console_end_status_line (console, cancellable, error))
        {
          console = NULL;
          goto out;
        }
      console = NULL;
    }

  if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  g_print ("Deleting ref '%s:%s'\n", origin_remote, origin_ref);
  ostree_repo_transaction_set_ref (repo, origin_remote, origin_ref, NULL);
  
  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;
  
#ifdef HAVE_PATCHED_HAWKEY_AND_LIBSOLV
  if (!rpmostree_print_treepkg_diff (sysroot, cancellable, error))
    goto out;
#endif
  
  ret = TRUE;
 out:
  if (console)
    (void) gs_console_end_status_line (console, NULL, NULL);
  return ret;
}
