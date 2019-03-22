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

/* XXX: This file is lifted from src/libostree/ostree-kernel-args.c, though there are some
 * new APIs. Should upstream those and dedupe. */

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
  if (eq == NULL)
    return NULL;

  // Note: key/val are in a single allocation block, so we don't free val.
  *eq = '\0';
  return eq+1;
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


static gboolean
strcmp0_equal (gconstpointer v1,
               gconstpointer v2)
{
  return g_strcmp0 (v1, v2) == 0;
}

/* Note: this function is newly added to the API */
GHashTable*
_ostree_kernel_arg_get_kargs_table (OstreeKernelArgs *kargs)
{

  if (kargs != NULL)
    return kargs->table;
  return NULL;
}

/* Note: this function is newly added to the API */
GPtrArray*
_ostree_kernel_arg_get_key_array (OstreeKernelArgs *kargs)
{
  if (kargs != NULL)
    return kargs->order;
  return NULL;
}

/*
 * Note: this function is newly added to the API
 *
 * _ostree_kernel_arg_new_replace:
 * @kargs: OstreeKernelArgs instance
 * @arg: a string argument
 * @error: error instance
 *
 * This function implements the basic logic behind key/value pair
 * replacement. Do note that the arg need to be properly formatted
 *
 * When replacing key with exact one value, the arg can be in
 * the form:
 * key, key=new_val, or key=old_val=new_val
 * The first one swaps the old_val with the key to an empty value
 * The second and third replace the old_val into the new_val
 *
 * When replacing key with multiple values, the arg can only be
 * in the form of:
 * key=old_val=new_val. Unless there is a special case where
 * there is an empty value associated with the key, then
 * key=new_val will work because old_val is empty. The empty
 * val will be swapped with the new_val in that case
 *
 * Returns: False if an error is set
 **/
gboolean
_ostree_kernel_args_new_replace (OstreeKernelArgs *kargs,
                                 const char       *arg,
                                 GError          **error)
{
  g_autofree char *arg_owned = g_strdup (arg);
  const char *key = arg_owned;
  const char *val = split_keyeq (arg_owned);

  GPtrArray *values = g_hash_table_lookup (kargs->table, key);
  if (!values)
    return glnx_throw (error, "No key '%s' found", key);
  g_assert_cmpuint (values->len, >, 0);

  /* first handle the case where the user just wants to replace an old value */
  if (val && strchr (val, '='))
    {
      g_autofree char *old_val = g_strdup (val);
      const char *new_val = split_keyeq (old_val);
      g_assert (new_val);

      guint i = 0;
      if (!g_ptr_array_find_with_equal_func (values, old_val, strcmp0_equal, &i))
        return glnx_throw (error, "No karg '%s=%s' found", key, old_val);

      g_clear_pointer (&values->pdata[i], g_free);
      values->pdata[i] = g_strdup (new_val);
      return TRUE;
    }

  /* can't know which val to replace without the old_val=new_val syntax */
  if (values->len > 1)
    return glnx_throw (error, "Multiple values for key '%s' found", key);

  g_clear_pointer (&values->pdata[0], g_free);
  values->pdata[0] = g_strdup (val);
  return TRUE;
}

/*
 *  Note: this is a new function added to the API
 *
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
_ostree_kernel_args_delete (OstreeKernelArgs  *kargs,
                            const char        *arg,
                            GError           **error)
{
  g_autofree char *arg_owned = g_strdup (arg);
  const char *key = arg_owned;
  const char *val = split_keyeq (arg_owned);

  GPtrArray *values = g_hash_table_lookup (kargs->table, key);
  if (!values)
    return glnx_throw (error, "No key '%s' found", key);
  g_assert_cmpuint (values->len, >, 0);

  /* special-case: we allow deleting by key only if there's only one val */
  if (values->len == 1)
    {
      /* but if a specific val was passed, check that it's the same */
      if (val && !strcmp0_equal (val, values->pdata[0]))
        return glnx_throw (error, "No karg '%s=%s' found", key, val);
      return _ostree_kernel_args_delete_key_entry (kargs, key, error);
    }

  /* multiple values, but just key supplied? error out */
  if (!val)
    return glnx_throw (error, "Multiple values for key '%s' found", key);

  guint i = 0;
  if (!g_ptr_array_find_with_equal_func (values, val, strcmp0_equal, &i))
    return glnx_throw (error, "No karg '%s' found", arg);

  g_ptr_array_remove_index (values, i);
  return TRUE;
}

/*
 * Note: this is a new function added to the API
 *
 * _ostree_kernel_args_delete_key_entry
 * @kargs: an OstreeKernelArgs instance
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
                                      const char       *key,
                                      GError          **error)
{
  if (!g_hash_table_remove (kargs->table, key))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to find kernel argument '%s'",
                   key);
      return FALSE;
    }

  /* Then remove the key from order table */
  guint key_index;
  g_assert (g_ptr_array_find_with_equal_func (kargs->order, key, g_str_equal, &key_index));
  g_assert (g_ptr_array_remove_index (kargs->order, key_index));
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

          if (value == NULL)
            g_ptr_array_add (strv, g_strconcat (key, NULL));
          else
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

          g_string_append (buf, key);
          if (value != NULL)
            {
              g_string_append_c (buf, '=');
              g_string_append (buf, value);
            }
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

