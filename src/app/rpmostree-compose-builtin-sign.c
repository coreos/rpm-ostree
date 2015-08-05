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
  GOptionContext *context = g_option_context_new ("- Use rpm-sign to sign an OSTree commit");
  gs_unref_object GFile *repopath = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object GFile *tmp_commitdata_file = NULL;
  gs_unref_object GFileIOStream *tmp_sig_stream = NULL;
  gs_unref_object GFile *tmp_sig_file = NULL;
  gs_unref_object GFileIOStream *tmp_commitdata_stream = NULL;
  GOutputStream *tmp_commitdata_output = NULL;
  gs_unref_object GInputStream *commit_data = NULL;
  gs_free char *checksum = NULL;
  gs_unref_variant GVariant *commit_variant = NULL;
  gs_unref_bytes GBytes *commit_bytes = NULL;
  
  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
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

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 checksum, &commit_variant, error))
    goto out;

  commit_bytes = g_variant_get_data_as_bytes (commit_variant);
  commit_data = (GInputStream*)g_memory_input_stream_new_from_bytes (commit_bytes);
  
  tmp_commitdata_file = g_file_new_tmp ("tmpsigXXXXXX", &tmp_commitdata_stream,
                                       error);
  if (!tmp_commitdata_file)
    goto out;

  tmp_commitdata_output = (GOutputStream*)g_io_stream_get_output_stream ((GIOStream*)tmp_commitdata_stream);
  if (g_output_stream_splice ((GOutputStream*)tmp_commitdata_output,
                              commit_data,
                              G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                              G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                              cancellable, error) < 0)
    goto out;

  tmp_sig_file = g_file_new_tmp ("tmpsigoutXXXXXX", &tmp_sig_stream, error);
  if (!tmp_sig_file)
    goto out;

  (void) g_io_stream_close ((GIOStream*)tmp_sig_stream, NULL, NULL);
                                  
  if (!gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                      cancellable, error,
                                      "rpm-sign",
                                      "--key", opt_key_id,
                                      "--detachsign", gs_file_get_path_cached (tmp_commitdata_file),
                                      "--output", gs_file_get_path_cached (tmp_sig_file),
                                      NULL))
    goto out;

  {
    char *sigcontent = NULL;
    gsize len;
    gs_unref_bytes GBytes *sigbytes = NULL;

    if (!g_file_load_contents (tmp_sig_file, cancellable, &sigcontent, &len, NULL,
                               error))
      goto out;

    sigbytes = g_bytes_new_take (sigcontent, len);

    if (!ostree_repo_append_gpg_signature (repo, checksum, sigbytes,
                                           cancellable, error))
      goto out;
  }

  g_print ("Successfully signed OSTree commit=%s with key=%s\n",
           checksum, opt_key_id);
  
  ret = TRUE;
 out:
  if (tmp_commitdata_file)
    (void) gs_file_unlink (tmp_commitdata_file, NULL, NULL);
  if (tmp_sig_file)
    (void) gs_file_unlink (tmp_sig_file, NULL, NULL);
  return ret;
}
