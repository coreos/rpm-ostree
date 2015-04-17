/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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

#include "rpmostree-compose-builtins.h"

#include "libgsystem.h"

static char *opt_repo_path;
static char *opt_key_id;
static char *opt_rev;

static GOptionEntry option_entries[] = {
  { "repo", 0, 0, G_OPTION_ARG_STRING, &opt_repo_path, "Repository path", "REPO" },
  { "key", 0, 0, G_OPTION_ARG_STRING, &opt_key_id, "Key ID", "KEY" },
  { "rev", 0, 0, G_OPTION_ARG_STRING, &opt_rev, "Revision to sign", "REV" },
  { NULL }
};

gboolean
rpmostree_compose_builtin_sign (int            argc,
                                char         **argv,
                                GCancellable  *cancellable,
                                GError       **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("- Sign an OSTree commit");
  gs_unref_object GFile *repopath = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_free char *checksum = NULL;
  
  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  if (!(opt_repo_path && opt_key_id && opt_rev))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing required argument");
      goto out;
    }

  repopath = g_file_new_for_path (opt_repo_path);
  repo = ostree_repo_new (repopath);
  if (!ostree_repo_open (repo, cancellable, error))
    goto out;

  if (!ostree_repo_resolve_rev (repo, opt_rev, FALSE, &checksum, error))
    goto out;

  if (!ostree_repo_sign_commit (repo, checksum, opt_key_id,
                                NULL, cancellable, error))
    goto out;

  g_print ("Successfully signed OSTree commit=%s with key=%s\n",
           checksum, opt_key_id);
  
  ret = TRUE;
 out:
  return ret;
}
