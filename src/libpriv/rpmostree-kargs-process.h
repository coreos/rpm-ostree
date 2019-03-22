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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _OstreeKernelArgs OstreeKernelArgs;

/*
 * Note: this is newly added to the API:
 * This flag is used to track the 'validity' and status of the arguments
 *
 * OSTREE_KERNEL_ARG_KEY_ONE_VALUE: means there is only one value associated with the key
 * OSTREE_KERNEL_ARG_REPLACE_NO_SECOND_SPLIT: means to tell 'replace function', the arg only need to be split once
 * OSTREE_KERNEL_ARG_FOUND_KEY_MULTI_VALUE: means the key found has multiple values associated with it
 *
 **/
typedef enum {
  OSTREE_KERNEL_ARG_KEY_ONE_VALUE = (1 << 0),
  OSTREE_KERNEL_ARG_FOUND_KEY_MULTI_VALUE = (1 << 1),
  OSTREE_KERNEL_ARG_REPLACE_NO_SECOND_SPLIT = (1 << 2),
} OstreeKernelArgQueryFlag;

GHashTable* _ostree_kernel_arg_get_kargs_table (OstreeKernelArgs *kargs);
GPtrArray* _ostree_kernel_arg_get_key_array (OstreeKernelArgs *kargs);

gboolean _ostree_ptr_array_find (GPtrArray *array, const char *arg, int *out_index);

OstreeKernelArgs *_ostree_kernel_args_new (void);
void _ostree_kernel_args_free (OstreeKernelArgs *kargs);
void _ostree_kernel_args_cleanup (void *loc);
void _ostree_kernel_args_replace_take (OstreeKernelArgs  *kargs,
                                       char              *key);
void _ostree_kernel_args_replace (OstreeKernelArgs  *kargs,
                                  const char        *key);
void _ostree_kernel_args_replace_argv (OstreeKernelArgs  *kargs,
                                       char             **argv);
void _ostree_kernel_args_append (OstreeKernelArgs  *kargs,
                                 const char     *key);
void _ostree_kernel_args_append_argv (OstreeKernelArgs  *kargs,
                                      char **argv);
void _ostree_kernel_args_append_argv_filtered (OstreeKernelArgs  *kargs,
                                               char **argv,
                                               char **prefixes);

gboolean _ostree_kernel_args_new_replace (OstreeKernelArgs *kargs,
                                          const char       *arg,
                                          GError          **error);

gboolean _ostree_kernel_args_delete (OstreeKernelArgs *kargs,
                                     const char       *arg,
                                     GError           **error);


gboolean _ostree_kernel_args_delete_key_entry (OstreeKernelArgs *kargs,
                                               const char       *key,
                                               GError          **error);


gboolean _ostree_kernel_args_append_proc_cmdline (OstreeKernelArgs *kargs,
                                                  GCancellable     *cancellable,
                                                  GError          **error);

void _ostree_kernel_args_parse_append (OstreeKernelArgs *kargs,
                                       const char *options);

const char *_ostree_kernel_args_get_last_value (OstreeKernelArgs *kargs, const char *key);

OstreeKernelArgs * _ostree_kernel_args_from_string (const char *options);

char ** _ostree_kernel_args_to_strv (OstreeKernelArgs *kargs);
char * _ostree_kernel_args_to_string (OstreeKernelArgs *kargs);

G_END_DECLS

