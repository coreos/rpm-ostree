/*
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "libglnx.h"
#include "rpmostree-editor.h"
#include "rpmostree-util.h"
#include <string.h>
#include <sys/wait.h>

#ifndef DEFAULT_EDITOR
#define DEFAULT_EDITOR "vi"
#endif

/* Logic pulled from git */

static const char *
get_editor (void)
{
  const char *editor = g_getenv ("OSTREE_EDITOR");
  const char *terminal = g_getenv ("TERM");
  int terminal_is_dumb = !terminal || g_str_equal (terminal, "dumb");

  if (!editor && !terminal_is_dumb)
    editor = g_getenv ("VISUAL");
  if (!editor)
    editor = g_getenv ("EDITOR");

  if (!editor && terminal_is_dumb)
    return NULL;

  if (!editor)
    editor = DEFAULT_EDITOR;

  return editor;
}

static char *
run_editor (GFile *file, GFileIOStream *io, const char *input, GCancellable *cancellable,
            GError **error)
{
  g_assert (file != NULL);
  g_assert (io != NULL);

  g_autofree char *content = NULL;

  const char *editor = get_editor ();
  if (editor == NULL)
    return (char *)glnx_null_throw (error, "Terminal is dumb, but EDITOR unset");

  GOutputStream *output = g_io_stream_get_output_stream (G_IO_STREAM (io));
  if (!g_output_stream_write_all (output, input, strlen (input), NULL, cancellable, error))
    return (char *)glnx_prefix_error_null (error, "Writing output content");

  if (!g_io_stream_close (G_IO_STREAM (io), cancellable, error))
    return (char *)glnx_prefix_error_null (error, "Closing stream");

  g_autofree char *args = NULL;
  {
    g_autofree char *quoted_file = g_shell_quote (gs_file_get_path_cached (file));
    args = g_strconcat (editor, " ", quoted_file, NULL);
  }

  glnx_unref_object GSubprocess *proc
      = g_subprocess_new (G_SUBPROCESS_FLAGS_STDIN_INHERIT, error, "/bin/sh", "-c", args, NULL);

  if (!g_subprocess_wait_check (proc, cancellable, error))
    return (char *)glnx_prefix_error_null (error, "Running editor '%s'", editor);

  content = glnx_file_get_contents_utf8_at (AT_FDCWD, gs_file_get_path_cached (file), NULL,
                                            cancellable, error);
  if (content == NULL)
    return (char *)glnx_prefix_error_null (error, "Reading content");

  return util::move_nullify (content);
}

char *
ot_editor_prompt (OstreeRepo *repo, const char *input, GCancellable *cancellable, GError **error)
{
  g_assert (input != NULL);

  g_autoptr (GFileIOStream) io = NULL;
  g_autoptr (GFile) file = g_file_new_tmp (NULL, &io, error);
  if (file == NULL)
    return (char *)glnx_prefix_error_null (error, "Creating temporary file");

  char *content = run_editor (file, io, input, cancellable, error);
  (void)g_file_delete (file, NULL, NULL);

  return content;
}
