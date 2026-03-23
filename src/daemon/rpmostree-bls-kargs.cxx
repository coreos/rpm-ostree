/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include "rpmostree-bls-kargs.h"
#include <libglnx.h>
#include <string.h>

/**
 * Compute the diff between old and new source kargs.
 *
 * Both @old_kargs and @new_kargs are space-separated kargs strings.
 * The function computes the set difference in both directions:
 *   - @out_to_add: kargs in @new_kargs but not in @old_kargs
 *   - @out_to_remove: kargs in @old_kargs but not in @new_kargs
 */
gboolean
rpmostree_bls_compute_source_diff (const char *old_kargs, const char *new_kargs, char ***out_to_add,
                                   char ***out_to_remove)
{
  g_autoptr (GHashTable) old_set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GHashTable) new_set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* Parse old kargs into a set */
  if (old_kargs != NULL && *old_kargs != '\0')
    {
      g_auto (GStrv) old_argv = g_strsplit (old_kargs, " ", -1);
      for (char **iter = old_argv; iter && *iter; iter++)
        {
          if (**iter != '\0')
            g_hash_table_add (old_set, g_strdup (*iter));
        }
    }

  /* Parse new kargs into a set */
  if (new_kargs != NULL && *new_kargs != '\0')
    {
      g_auto (GStrv) new_argv = g_strsplit (new_kargs, " ", -1);
      for (char **iter = new_argv; iter && *iter; iter++)
        {
          if (**iter != '\0')
            g_hash_table_add (new_set, g_strdup (*iter));
        }
    }

  /* Compute to_add: in new but not in old */
  g_autoptr (GPtrArray) to_add = g_ptr_array_new_with_free_func (g_free);
  {
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init (&iter, new_set);
    while (g_hash_table_iter_next (&iter, &key, NULL))
      {
        if (!g_hash_table_contains (old_set, key))
          g_ptr_array_add (to_add, g_strdup ((char *)key));
      }
  }

  /* Compute to_remove: in old but not in new */
  g_autoptr (GPtrArray) to_remove = g_ptr_array_new_with_free_func (g_free);
  {
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init (&iter, old_set);
    while (g_hash_table_iter_next (&iter, &key, NULL))
      {
        if (!g_hash_table_contains (new_set, key))
          g_ptr_array_add (to_remove, g_strdup ((char *)key));
      }
  }

  gboolean changed = (to_add->len > 0 || to_remove->len > 0);

  if (out_to_add)
    {
      g_ptr_array_add (to_add, NULL);
      GPtrArray *arr = static_cast<GPtrArray *> (g_steal_pointer (&to_add));
      *out_to_add = (char **)g_ptr_array_free (arr, FALSE);
    }

  if (out_to_remove)
    {
      g_ptr_array_add (to_remove, NULL);
      GPtrArray *arr = static_cast<GPtrArray *> (g_steal_pointer (&to_remove));
      *out_to_remove = (char **)g_ptr_array_free (arr, FALSE);
    }

  return changed;
}
