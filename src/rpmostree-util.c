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
#include <stdio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <gio/gunixoutputstream.h>

#include "rpmostree-util.h"
#include "libgsystem.h"

void
_rpmostree_set_error_from_errno (GError    **error,
                                 gint        errsv)
{
  g_set_error_literal (error,
                       G_IO_ERROR,
                       g_io_error_from_errno (errsv),
                       g_strerror (errsv));
  errno = errsv;
}

void
_rpmostree_set_prefix_error_from_errno (GError     **error,
                                        gint         errsv,
                                        const char  *format,
                                        ...)
{
  gs_free char *formatted = NULL;
  va_list args;
  
  va_start (args, format);
  formatted = g_strdup_vprintf (format, args);
  va_end (args);
  
  _rpmostree_set_error_from_errno (error, errsv);
  g_prefix_error (error, "%s", formatted);
  errno = errsv;
}

void
_rpmostree_perror_fatal (const char *message)
{
  perror (message);
  exit (1);
}

gboolean
_rpmostree_util_enumerate_directory_allow_noent (GFile               *dirpath,
						 const char          *queryargs,
						 GFileQueryInfoFlags  queryflags,
						 GFileEnumerator    **out_direnum,
						 GCancellable        *cancellable,
						 GError             **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gs_unref_object GFileEnumerator *ret_direnum = NULL;

  ret_direnum = g_file_enumerate_children (dirpath, queryargs, queryflags,
                                           cancellable, &temp_error);
  if (!ret_direnum)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret = TRUE;
        }
      else
        g_propagate_error (error, temp_error);

      goto out;
    }

  ret = TRUE;
  gs_transfer_out_value (out_direnum, &ret_direnum);
 out:
  return ret;
}

gboolean
_rpmostree_file_load_contents_utf8_allow_noent (GFile          *path,
                                                char          **out_contents,
                                                GCancellable   *cancellable,
                                                GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gs_free char *ret_contents = NULL;

  ret_contents = gs_file_load_contents_utf8 (path, cancellable, &temp_error);
  if (!ret_contents)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  gs_transfer_out_value (out_contents, &ret_contents);
 out:
  return ret;
}

gboolean
_rpmostree_util_update_checksum_from_file (GChecksum    *checksum,
                                           GFile        *src,
                                           GCancellable *cancellable,
                                           GError      **error)
{
  gboolean ret = FALSE;
  gsize bytes_read;
  char buf[4096];
  gs_unref_object GInputStream *filein = NULL;

  filein = (GInputStream*)g_file_read (src, cancellable, error);
  if (!filein)
    goto out;

  do
    {
      if (!g_input_stream_read_all (filein, buf, sizeof(buf), &bytes_read,
                                    cancellable, error))
        goto out;

      g_checksum_update (checksum, (guint8*)buf, bytes_read);
    }
  while (bytes_read > 0);

  ret = TRUE;
 out:
  return ret;
}

gboolean
_rpmostree_sync_wait_on_pid (pid_t          pid,
                             GError       **error)
{
  gboolean ret = FALSE;
  pid_t r;
  int estatus;

  do
    r = waitpid (pid, &estatus, 0);
  while (G_UNLIKELY (r == -1 && errno == EINTR));

  if (r == -1)
    {
      _rpmostree_set_prefix_error_from_errno (error, errno, "waitpid: ");
      goto out;
    }

  if (!g_spawn_check_exit_status (estatus, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
