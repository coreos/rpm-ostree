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
 * Core routines wrapping libarchive unpacking of an RPM.
 */

#include "config.h"

#include "rpmostree-core.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker-core.h"
#include <gio/gunixinputstream.h>
#include <grp.h>
#include <pwd.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmts.h>
#include <stdlib.h>
#include <string.h>

/**
 * throw_libarchive_error:
 * @ar: An archive
 * @error: GError
 * @prefix: Optional error prefix
 *
 * Transform a libarchive error into a Glib one, then clean up the input archive.
 *
 * Return: Always NULL
 */
static struct archive *
throw_libarchive_error (struct archive *ar, GError **error, char const *prefix)
{
  g_assert (ar != NULL);

  const char *err_string = archive_error_string (ar);
  g_assert (err_string != NULL);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, err_string);

  if (prefix != NULL)
    glnx_prefix_error (error, "%s", prefix);

  return NULL;
}

typedef int (*archive_setup_func) (struct archive *);

/**
 * rpmostree_unpack_rpm2cpio:
 * @fd: An open file descriptor for an RPM package
 * @error: GError
 *
 * Parse CPIO content of @fd via libarchive.  Note that the CPIO data
 * does not capture all relevant filesystem content; for example,
 * filesystem capabilities are part of a separate header, etc.
 */
struct archive *
rpmostree_unpack_rpm2cpio (int fd, GError **error)
{
  g_autoptr (archive) ar = archive_read_new ();
  if (ar == NULL)
    return (struct archive *)glnx_null_throw (error,
                                              "Failed to initialize rpm2cpio archive object");

  /* We only do the subset necessary for RPM */
  {
    archive_setup_func archive_setup_funcs[]
        = { archive_read_support_filter_rpm,   archive_read_support_filter_lzma,
            archive_read_support_filter_gzip,  archive_read_support_filter_xz,
            archive_read_support_filter_bzip2,
#ifdef HAVE_LIBARCHIVE_ZSTD
            archive_read_support_filter_zstd,
#endif
            archive_read_support_format_cpio };

    for (guint i = 0; i < G_N_ELEMENTS (archive_setup_funcs); i++)
      {
        if (archive_setup_funcs[i](ar) != ARCHIVE_OK)
          return throw_libarchive_error (ar, error, "Setting up rpm2cpio");
      }
  }

  if (archive_read_open_fd (ar, fd, 10240) != ARCHIVE_OK)
    return throw_libarchive_error (ar, error, "Reading rpm2cpio");

  return util::move_nullify (ar);
}
