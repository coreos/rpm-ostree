/*
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

#include "rpmostree-kargs-process.h"
#include "libglnx.h"

#include <string.h>

struct _OstreeKernelArgs {
  GPtrArray  *order;
  GHashTable *table;
};

static char *
split_keyeq (char *arg)
{
  char *eq;

  eq = strchr (arg, '=');
  if (eq)
    {
      /* Note key/val are in one malloc block,
       * so we don't free val...
       */
      *eq = '\0';
      return eq+1;
    }
  else
    {
      /* ...and this allows us to insert a constant
       * string.
       */
      return "";
    }
}

static gboolean
_arg_has_prefix (const char *arg,
                 char      **prefixes)
{
  char **strviter;

  for (strviter = prefixes; strviter && *strviter; strviter++)
    {
      const char *prefix = *strviter;

      if (g_str_has_prefix (arg, prefix))
        return TRUE;
    }

  return FALSE;
}

OstreeKernelArgs *
_ostree_kernel_args_new (void)
{
  OstreeKernelArgs *ret;
  ret = g_new0 (OstreeKernelArgs, 1);
  ret->order = g_ptr_array_new_with_free_func (g_free);
  ret->table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                      NULL, (GDestroyNotify)g_ptr_array_unref);
  return ret;
}

static void
_ostree_kernel_arg_autofree (OstreeKernelArgs *kargs)
{
  if (!kargs)
    return;
  g_ptr_array_unref (kargs->order);
  g_hash_table_unref (kargs->table);
  g_free (kargs);
}

void
_ostree_kernel_args_cleanup (void *loc)
{
  _ostree_kernel_arg_autofree (*((OstreeKernelArgs**)loc));
}


/*
 * _ostree_ptr_array_find
 *  @array: a GPtrArray instance
 *  @val: a string value
 *  @out_index: the returned index
 *
 *  Note: This is a temp replacement for 'g_ptr_array_find_with_equal_func'
 *  since that function was recently introduced in Glib (version 2.54),
 *  the version is still not updated upstream yet,  thus tempoarily using
 *  this as a replacement
 *
 *  Returns: False if can not find the index
 *
 **/
static
gboolean _ostree_ptr_array_find (GPtrArray       *array,
                        const char      *val,
                        int             *out_index)
{

  for (int counter = 0; counter < array->len; counter++)
    {
      const char *temp_val = array->pdata[counter];
      if  (g_str_equal (val, temp_val))
        {
          *out_index = counter;
          return TRUE;
        }

    }
    return FALSE;
}


/*
 *  _ostree_kernel_args_delete:
 *  @kargs: a OstreeKernelArgs instance
 *  @arg: key or key/value pair for deletion
 *  @error: an GError instance
 *
 *  There are few scenarios being handled for deletion:
 *
 *  1: for input arg with a single key(i.e without = for split),
 *  the key/value pair will be deleted if there is only
 *  one value that is associated with the key
 *
 *  2: for input arg wth key/value pair, the specific key
 *  value pair will be deleted from the pointer array
 *  if those exist.
 *
 *  3: If the found key has only one value
 *  associated with it, the key entry in the table will also
 *  be removed, and the key will be removed from order table
 *
 *  Returns: False if an error is set,
 *
 **/
gboolean
_ostree_kernel_args_delete (OstreeKernelArgs *kargs,
                            const char       *arg,
                            GError           **error)
{
  /* Make a duplicate of the passed in arg for manipulation */
  g_autofree char *duped = g_strdup (arg);
  const char *val = split_keyeq (duped);

  GPtrArray *values = g_hash_table_lookup (kargs->table, duped);

  if (!values)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "The key is not found, type rpm-ostree ex kargs to see what args are there");
      return FALSE;
    }

  /* if the value is empty, directly check if we can remove the key */
  if (!*val)
    {

      if (values->len == 1)
        return _ostree_kernel_args_delete_key_entry (kargs, duped, error);
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unable to delete %s with multiple values associated with it, try delete by key=value format", duped);
          return FALSE;
        }
    }

  int value_index;
  /* See if the value exist in the values array,
   * if it is, we return its index */
  if (!_ostree_ptr_array_find (values, val,
                               &value_index))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "There is no %s value for key %s",
                   val, duped);
      return FALSE;
    }

  /* We confirmed the last entry is valid to remove
   * we remove the entry from table, and remove the entry
   * from order array */
  if (values->len == 1)
    return _ostree_kernel_args_delete_key_entry (kargs, duped, error);

  if (!g_ptr_array_remove_index (values, value_index))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Remove failure happens");
      return FALSE;
    }

  return TRUE;

}

/*
 * _ostree_kernel_args_delete_key_entry
 * @kargs: an OstreeKernelArgs intanc
 * @key: the key to remove
 * @error: an GError instance
 *
 * This function removes the key entry from the hashtable
 * as well from the order pointer array inside kargs
 *
 * Note: since both table and order inside kernel args
 * are with free function, no extra free functions are
 * being called as they are done automatically by GLib
 *
 * Returns: False if key does not exist/ key has mutliple
 * values associated with it
 **/
gboolean
_ostree_kernel_args_delete_key_entry (OstreeKernelArgs *kargs,
                                      char             *key,
                                      GError          **error)
{
  if (!g_hash_table_remove(kargs->table, key))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to remove %s from hashtable",
                   key);
      return FALSE;
    }

  /* Then remove the key from order table */
  int key_index;
  if (!_ostree_ptr_array_find (kargs->order, key,
                              &key_index))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to find %s in the array",
                   key);
      return FALSE;
    }
  if (!g_ptr_array_remove_index (kargs->order, key_index))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to remove %s from order array",
                   key);
      return FALSE;
    }
  return TRUE;
}


void
_ostree_kernel_args_replace_take (OstreeKernelArgs   *kargs,
                                  char               *arg)
{
  gboolean existed;
  GPtrArray *values = g_ptr_array_new_with_free_func (g_free);
  const char *value = split_keyeq (arg);
  gpointer old_key;

  g_ptr_array_add (values, g_strdup (value));
  existed = g_hash_table_lookup_extended (kargs->table, arg, &old_key, NULL);

  if (existed)
    {
      g_hash_table_replace (kargs->table, old_key, values);
      g_free (arg);
    }
  else
    {
      g_ptr_array_add (kargs->order, arg);
      g_hash_table_replace (kargs->table, arg, values);
    }
}

void
_ostree_kernel_args_replace (OstreeKernelArgs  *kargs,
                             const char        *arg)
{
  _ostree_kernel_args_replace_take (kargs, g_strdup (arg));
}

void
_ostree_kernel_args_append (OstreeKernelArgs  *kargs,
                            const char        *arg)
{
  gboolean existed = TRUE;
  GPtrArray *values;
  char *duped = g_strdup (arg);
  const char *val = split_keyeq (duped);

  values = g_hash_table_lookup (kargs->table, duped);
  if (!values)
    {
      values = g_ptr_array_new_with_free_func (g_free);
      existed = FALSE;
    }

  g_ptr_array_add (values, g_strdup (val));

  if (!existed)
    {
      g_hash_table_replace (kargs->table, duped, values);
      g_ptr_array_add (kargs->order, duped);
    }
  else
    {
      g_free (duped);
    }
}

void
_ostree_kernel_args_replace_argv (OstreeKernelArgs  *kargs,
                                  char             **argv)
{
  char **strviter;

  for (strviter = argv; strviter && *strviter; strviter++)
    {
      const char *arg = *strviter;
      _ostree_kernel_args_replace (kargs, arg);
    }
}

void
_ostree_kernel_args_append_argv_filtered (OstreeKernelArgs  *kargs,
                                          char             **argv,
                                          char             **prefixes)
{
  char **strviter;

  for (strviter = argv; strviter && *strviter; strviter++)
    {
      const char *arg = *strviter;

      if (!_arg_has_prefix (arg, prefixes))
        _ostree_kernel_args_append (kargs, arg);
    }
}

void
_ostree_kernel_args_append_argv (OstreeKernelArgs  *kargs,
                                 char             **argv)
{
  _ostree_kernel_args_append_argv_filtered (kargs, argv, NULL);
}

gboolean
_ostree_kernel_args_append_proc_cmdline (OstreeKernelArgs *kargs,
                                         GCancellable     *cancellable,
                                         GError          **error)
{
  g_autoptr(GFile) proc_cmdline_path = g_file_new_for_path ("/proc/cmdline");
  g_autofree char *proc_cmdline = NULL;
  gsize proc_cmdline_len = 0;
  g_auto(GStrv) proc_cmdline_args = NULL;
  /* When updating the filter list don't forget to update the list in the tests
   * e.g. tests/test-admin-deploy-karg.sh and
   * tests/test-admin-instutil-set-kargs.sh
   */
  char *filtered_prefixes[] = { "BOOT_IMAGE=", /* GRUB 2 */
                                "initrd=", /* sd-boot */
                                NULL };

  if (!g_file_load_contents (proc_cmdline_path, cancellable,
                             &proc_cmdline, &proc_cmdline_len,
                             NULL, error))
    return FALSE;

  g_strchomp (proc_cmdline);

  proc_cmdline_args = g_strsplit (proc_cmdline, " ", -1);
  _ostree_kernel_args_append_argv_filtered (kargs, proc_cmdline_args,
                                            filtered_prefixes);

  return TRUE;
}

void
_ostree_kernel_args_parse_append (OstreeKernelArgs *kargs,
                                  const char       *options)
{
  char **args = NULL;
  char **iter;

  if (!options)
    return;

  args = g_strsplit (options, " ", -1);
  for (iter = args; *iter; iter++)
    {
      char *arg = *iter;
      _ostree_kernel_args_append (kargs, arg);
    }
  g_strfreev (args);
}

OstreeKernelArgs *
_ostree_kernel_args_from_string (const char *options)
{
  OstreeKernelArgs *ret;

  ret = _ostree_kernel_args_new ();
  _ostree_kernel_args_parse_append (ret, options);

  return ret;
}

char **
_ostree_kernel_args_to_strv (OstreeKernelArgs *kargs)
{
  GPtrArray *strv = g_ptr_array_new ();
  guint i;

  for (i = 0; i < kargs->order->len; i++)
    {
      const char *key = kargs->order->pdata[i];
      GPtrArray *values = g_hash_table_lookup (kargs->table, key);
      guint j;

      g_assert (values != NULL);

      for (j = 0; j < values->len; j++)
        {
          const char *value = values->pdata[j];

          g_ptr_array_add (strv, g_strconcat (key, "=", value, NULL));
        }
    }
  g_ptr_array_add (strv, NULL);

  return (char**)g_ptr_array_free (strv, FALSE);
}

char *
_ostree_kernel_args_to_string (OstreeKernelArgs *kargs)
{
  GString *buf = g_string_new ("");
  gboolean first = TRUE;
  guint i;

  for (i = 0; i < kargs->order->len; i++)
    {
      const char *key = kargs->order->pdata[i];
      GPtrArray *values = g_hash_table_lookup (kargs->table, key);
      guint j;

      g_assert (values != NULL);

      for (j = 0; j < values->len; j++)
        {
          const char *value = values->pdata[j];

          if (first)
            first = FALSE;
          else
            g_string_append_c (buf, ' ');

          if (value && *value)
            {
              g_string_append (buf, key);
              g_string_append_c (buf, '=');
              g_string_append (buf, value);
            }
          else
            g_string_append (buf, key);
        }
    }

  return g_string_free (buf, FALSE);
}

const char *
_ostree_kernel_args_get_last_value (OstreeKernelArgs *kargs, const char *key)
{
  GPtrArray *values = g_hash_table_lookup (kargs->table, key);

  if (!values)
    return NULL;

  g_assert (values->len > 0);
  return (char*)values->pdata[values->len-1];
}

