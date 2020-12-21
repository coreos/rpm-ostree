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

#include "rpmostree-json-parsing.h"
#include "libglnx.h"

#include <string.h>
#include <glib-unix.h>

gboolean
_rpmostree_jsonutil_object_get_optional_string_member (JsonObject     *object,
                                                       const char     *member_name,
                                                       const char    **out_value,
                                                       GError        **error)
{
  *out_value = NULL;

  if (!object)
    return TRUE;

  JsonNode *node = json_object_get_member (object, member_name);
  if (node != NULL)
    {
      *out_value = json_node_get_string (node);
      if (!*out_value)
        return glnx_throw (error, "Member '%s' is not a string", member_name);
    }

  return TRUE;
}

const char *
_rpmostree_jsonutil_object_require_string_member (JsonObject     *object,
                                                  const char     *member_name,
                                                  GError        **error)
{
  const char *ret;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (object, member_name, &ret, error))
    return NULL;
  if (!ret)
    return (char*)glnx_null_throw (error, "Member '%s' not found", member_name);
  return ret;
}

static gboolean
_jsonutil_node_check_int (JsonNode *node)
{
  if (!node)
    return FALSE;

  if (json_node_get_value_type (node) != G_TYPE_INT64)
    return FALSE;

  return TRUE;
}

gboolean
_rpmostree_jsonutil_object_get_optional_int_member (JsonObject     *object,
                                                    const char     *member_name,
                                                    gint64         *out_value,
                                                    gboolean       *found,
                                                    GError        **error)
{
  if (found)
    *found = FALSE;
  *out_value = 0;

  if (!object)
    return TRUE;

  JsonNode *node = json_object_get_member (object, member_name);
  if (node != NULL)
    {
      if (!_jsonutil_node_check_int (node))
        return glnx_throw (error, "Member '%s' is not an integer", member_name);
      if (found)
        *found = TRUE;
      *out_value = json_node_get_int (node);
    }

  return TRUE;
}

gboolean
_rpmostree_jsonutil_object_require_int_member (JsonObject     *object,
                                               const char     *member_name,
                                               gint64         *out_val,
                                               GError        **error)
{
  gboolean found;
  if (!_rpmostree_jsonutil_object_get_optional_int_member (object, member_name, out_val, &found, error))
    return FALSE;
  if (!found)
    return glnx_throw (error, "Member '%s' not found", member_name);
  return TRUE;
}

gboolean
_rpmostree_jsonutil_object_get_optional_boolean_member (JsonObject     *object,
                                                       const char     *member_name,
                                                       gboolean       *out_value,
                                                       GError        **error)
{
  if (!object)
    return TRUE;

  JsonNode *node = json_object_get_member (object, member_name);
  if (node != NULL)
    {
      if (json_node_get_value_type (node) != G_TYPE_BOOLEAN)
        return glnx_throw (error, "Member '%s' is not a boolean", member_name);
      *out_value = json_node_get_boolean (node);
    }

  return TRUE;
}

const char *
_rpmostree_jsonutil_array_require_string_element (JsonArray      *array,
                                                  guint           i,
                                                  GError        **error)
{
  const char *ret = json_array_get_string_element (array, i);
  if (!ret)
    return (char*)glnx_null_throw (error, "Element at index %u is not a string", i);
  return ret;
}

gboolean
_rpmostree_jsonutil_array_require_int_element (JsonArray      *array,
                                               guint           i,
                                               gint64         *out_val,
                                               GError        **error)
{
  JsonNode *node = json_array_get_element (array, i);

  if (!_jsonutil_node_check_int (node))
    return glnx_throw (error, "Element at index %u is not an integer", i);

  *out_val = json_array_get_int_element (array, i);
  return TRUE;
}

gboolean
_rpmostree_jsonutil_append_string_array_to (JsonObject   *object,
                                            const char   *member_name,
                                            GPtrArray    *array,
                                            GError      **error)
{
  JsonArray *jarray = json_object_get_array_member (object, member_name);

  if (!jarray)
    return glnx_throw (error, "No member '%s' found", member_name);

  guint len = json_array_get_length (jarray);
  for (guint i = 0; i < len; i++)
    {
      const char *v = _rpmostree_jsonutil_array_require_string_element (jarray, i, error);
      if (!v)
        return FALSE;
      g_ptr_array_add (array, g_strdup (v));
    }

  return TRUE;
}

GHashTable *
_rpmostree_jsonutil_jsarray_strings_to_set (JsonArray  *array)
{
  GHashTable *ret = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  const guint len = json_array_get_length (array);

  for (guint i = 0; i < len; i++)
    {
      const char *elt = json_array_get_string_element (array, i);
      g_hash_table_add (ret, g_strdup (elt));
    }

  return ret;
}
