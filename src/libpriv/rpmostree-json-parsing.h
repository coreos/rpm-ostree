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

#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

gboolean
_rpmostree_jsonutil_object_get_optional_string_member (JsonObject     *object,
                                                       const char     *member_name,
                                                       const char    **out_value,
                                                       GError        **error);

const char *
_rpmostree_jsonutil_object_require_string_member (JsonObject     *object,
                                                  const char     *member_name,
                                                  GError        **error);

gboolean
_rpmostree_jsonutil_object_get_optional_int_member (JsonObject     *object,
                                                    const char     *member_name,
                                                    gint64         *out_value,
                                                    gboolean       *found,
                                                    GError        **error);

gboolean
_rpmostree_jsonutil_object_require_int_member (JsonObject     *object,
                                               const char     *member_name,
                                               gint64         *out_val,
                                               GError        **error);

gboolean
_rpmostree_jsonutil_object_get_optional_boolean_member (JsonObject     *object,
                                                       const char     *member_name,
                                                       gboolean       *out_value,
                                                       GError        **error);

const char *
_rpmostree_jsonutil_array_require_string_element (JsonArray      *array,
                                                  guint           i,
                                                  GError        **error);

gboolean
_rpmostree_jsonutil_array_require_int_element (JsonArray      *array,
                                               guint           i,
                                               gint64         *out_val,
                                               GError        **error);

gboolean
_rpmostree_jsonutil_append_string_array_to (JsonObject   *object,
                                            const char   *member_name,
                                            GPtrArray    *array,
                                            GError      **error);

GHashTable *
_rpmostree_jsonutil_jsarray_strings_to_set (JsonArray  *array);


G_END_DECLS
