/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * Implements unpacking an RPM.  The design here is to reuse
 * libarchive's RPM support for most of it.  We do however need to
 * look at file capabilities, which are part of the header.
 *
 * Hence we end up with two file descriptors open.
 */

#include "config.h"

#include "rpmostree-ostree-libarchive-copynpaste.h"
#include "ostree-libarchive-input-stream.h"

#include <string.h>

gboolean
rpmostree_split_path_ptrarray_validate (const char *path,
                                        GPtrArray  **out_components,
                                        GError     **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) ret_components = NULL;

  if (strlen (path) > PATH_MAX)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Path '%s' is too long", path);
      goto out;
    }

  ret_components = g_ptr_array_new_with_free_func (g_free);

  do
    {
      const char *p = strchr (path, '/');
      g_autofree char *component = NULL;

      if (!p)
        {
          component = g_strdup (path);
          path = NULL;
        }
      else
        {
          component = g_strndup (path, p - path);
          path = p + 1;
        }

      if (!component[0])
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid empty component in path '%s'", path);
          goto out;
        }
      if (g_str_equal (component, ".") ||
          g_str_equal (component, ".."))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid special element '.' or '..' in path %s", path);
          goto out;
        }

      g_ptr_array_add (ret_components, (char*)g_steal_pointer (&component));
    } while (path && *path);

  ret = TRUE;
  *out_components = g_steal_pointer (&ret_components);
 out:
  return ret;
}

static GFileType
gfile_type_for_mode (guint32 mode)
{
  if (S_ISDIR (mode))
    return G_FILE_TYPE_DIRECTORY;
  else if (S_ISREG (mode))
    return G_FILE_TYPE_REGULAR;
  else if (S_ISLNK (mode))
    return G_FILE_TYPE_SYMBOLIC_LINK;
  else if (S_ISBLK (mode) || S_ISCHR(mode) || S_ISFIFO(mode))
    return G_FILE_TYPE_SPECIAL;
  else
    return G_FILE_TYPE_UNKNOWN;
}

static GFileInfo *
_ostree_header_gfile_info_new (mode_t mode, uid_t uid, gid_t gid)
{
  GFileInfo *ret = g_file_info_new ();
  g_file_info_set_attribute_uint32 (ret, "standard::type", gfile_type_for_mode (mode));
  g_file_info_set_attribute_boolean (ret, "standard::is-symlink", S_ISLNK (mode));
  g_file_info_set_attribute_uint32 (ret, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (ret, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (ret, "unix::mode", mode);
  return ret;
}

GFileInfo *
_rpmostree_libarchive_to_file_info (struct archive_entry *entry)
{
  g_autoptr(GFileInfo) ret = NULL;
  struct stat st;

  st = *archive_entry_stat (entry);

  if (S_ISDIR (st.st_mode))
    {
      /* Always ensure we can write and execute directories...since
       * this content should ultimately be read-only entirely, we're
       * just breaking things by dropping write permissions during
       * builds.
       */
      st.st_mode |= 0700;
    }

  ret = _ostree_header_gfile_info_new (st.st_mode, st.st_uid, st.st_gid);

  if (S_ISREG (st.st_mode))
    g_file_info_set_attribute_uint64 (ret, "standard::size", st.st_size);
  if (S_ISLNK (st.st_mode))
    g_file_info_set_attribute_byte_string (ret, "standard::symlink-target", archive_entry_symlink (entry));

  return g_steal_pointer (&ret);
}

gboolean
_rpmostree_import_libarchive_entry_file (OstreeRepo           *repo,
                                         struct archive       *a,
                                         struct archive_entry *entry,
                                         GFileInfo            *file_info,
                                         GVariant             *xattrs,
                                         guchar              **out_csum,
                                         GCancellable         *cancellable,
                                         GError              **error)
{
  gboolean ret = FALSE;
  g_autoptr(GInputStream) file_object_input = NULL;
  g_autoptr(GInputStream) archive_stream = NULL;
  guint64 length;
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    archive_stream = _ostree_libarchive_input_stream_new (a);
  
  if (!ostree_raw_file_to_content_stream (archive_stream, file_info, xattrs,
                                          &file_object_input, &length, cancellable, error))
    goto out;
  
  if (!ostree_repo_write_content (repo, NULL, file_object_input, length, out_csum,
                                  cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
