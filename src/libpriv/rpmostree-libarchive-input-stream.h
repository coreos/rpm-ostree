/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#pragma once

#include <archive.h>
#include <archive_entry.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define RPM_OSTREE_TYPE_LIBARCHIVE_INPUT_STREAM (_rpm_ostree_libarchive_input_stream_get_type ())
#define RPM_OSTREE_LIBARCHIVE_INPUT_STREAM(o)                                                      \
  (G_TYPE_CHECK_INSTANCE_CAST ((o), RPM_OSTREE_TYPE_LIBARCHIVE_INPUT_STREAM,                       \
                               RpmOstreeLibarchiveInputStream))
#define RPM_OSTREE_LIBARCHIVE_INPUT_STREAM_CLASS(k)                                                \
  (G_TYPE_CHECK_CLASS_CAST ((k), RPM_OSTREE_TYPE_LIBARCHIVE_INPUT_STREAM,                          \
                            RpmOstreeLibarchiveInputStreamClass))
#define RPM_OSTREE_IS_LIBARCHIVE_INPUT_STREAM(o)                                                   \
  (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPM_OSTREE_TYPE_LIBARCHIVE_INPUT_STREAM))
#define RPM_OSTREE_IS_LIBARCHIVE_INPUT_STREAM_CLASS(k)                                             \
  (G_TYPE_CHECK_CLASS_TYPE ((k), RPM_OSTREE_TYPE_LIBARCHIVE_INPUT_STREAM))
#define RPM_OSTREE_LIBARCHIVE_INPUT_STREAM_GET_CLASS(o)                                            \
  (G_TYPE_INSTANCE_GET_CLASS ((o), RPM_OSTREE_TYPE_LIBARCHIVE_INPUT_STREAM,                        \
                              RpmOstreeLibarchiveInputStreamClass))

typedef struct _RpmOstreeLibarchiveInputStream RpmOstreeLibarchiveInputStream;
typedef struct _RpmOstreeLibarchiveInputStreamClass RpmOstreeLibarchiveInputStreamClass;
typedef struct _RpmOstreeLibarchiveInputStreamPrivate RpmOstreeLibarchiveInputStreamPrivate;

struct _RpmOstreeLibarchiveInputStream
{
  GInputStream parent_instance;

  /*< private >*/
  RpmOstreeLibarchiveInputStreamPrivate *priv;
};

struct _RpmOstreeLibarchiveInputStreamClass
{
  GInputStreamClass parent_class;

  /*< private >*/
  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType _rpm_ostree_libarchive_input_stream_get_type (void) G_GNUC_CONST;

GInputStream *_rpm_ostree_libarchive_input_stream_new (struct archive *a);

G_END_DECLS
