/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat Inc.
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

#include "rpmostree-ex-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-dbus-helpers.h"

#include <libglnx.h>

static gboolean opt_editor;
static gboolean opt_import_proc;
static char **opt_kernel_delete_strings;
static char **opt_kernel_append_strings;
static char **opt_kernel_replace_strings;
static char  *opt_osname;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operation on provided OSNAME", "OSNAME" },
  { "append", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_append_strings, "Append kernel argument; useful with e.g. console= that can be used multiple times, empty value is allowed", "KEY=VALUE" },
  { "replace", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_replace_strings, "Replace existing kernel argument, the user is also able to replace KEY=VALUE for a single key/value pair ", "KEY=VALUE=NEWVALUE" },
  { "delete", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_delete_strings, "Delete a specific kernel argument value or can delete the entire key for a single key/value pair", "KEY=VALUE"},
  { "import-proc-cmdline", 0, 0, G_OPTION_ARG_NONE, &opt_import_proc, "Instead of modifying old kernel arguments, we modify args from current /proc/cmdline", NULL },
  { "editor", 0, 0, G_OPTION_ARG_NONE, &opt_editor, "Pops up an editor for users to change arguments", NULL },
  { NULL }
};

static GVariant *
get_kargs_option_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "import-proc-cmdline", "b", opt_import_proc);

  return g_variant_dict_end (&dict);
}

int
rpmostree_ex_builtin_kargs (int            argc,
                            char         **argv,
                            RpmOstreeCommandInvocation *invocation,
                            GCancellable  *cancellable,
                            GError       **error)
{
  _cleanup_peer_ GPid peer_pid = 0;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new("");
  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       error))
    return EXIT_FAILURE;

  if (opt_editor && (opt_import_proc || opt_kernel_delete_strings ||
      opt_kernel_replace_strings ||  opt_kernel_append_strings))
    {
      /* We want editor command to achieve all the other functionalities */
      /* Thus erroring out ahead of time when multiple options exist */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify --editor with other options");
      return EXIT_FAILURE;
    }
  if (opt_kernel_delete_strings && opt_kernel_replace_strings)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --delete and --replace");
      return EXIT_FAILURE;
    }
  if (opt_kernel_delete_strings && opt_kernel_append_strings)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --delete and --append");
      return EXIT_FAILURE;
    }

  /* We do not want editor option to be handled via daemon */
  if (opt_editor)
    {
      ;
    }
  else
    {
      glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
      if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                    cancellable, &os_proxy, error))
        return EXIT_FAILURE;

      g_autofree char *transaction_address = NULL;
      char *empty_strv[] = {NULL};

      /* dbus does not allow empty string arrays, assign it
       * to be empty string array here to prevent erroring out */
      if (!opt_kernel_replace_strings)
        opt_kernel_replace_strings = empty_strv;
      if (!opt_kernel_append_strings)
        opt_kernel_append_strings = empty_strv;
      if (!opt_kernel_delete_strings)
        opt_kernel_delete_strings = empty_strv;

      /* call the generearted dbus-function */
      if (!rpmostree_os_call_kernel_args_sync (os_proxy,
                                               (const char* const*) opt_kernel_append_strings,
                                               (const char* const*) opt_kernel_replace_strings,
                                               (const char* const*) opt_kernel_delete_strings,
                                               get_kargs_option_variant (),
                                               &transaction_address,
                                               cancellable,
                                               error))
        return EXIT_FAILURE;

      if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                    transaction_address,
                                                    cancellable,
                                                    error))
        return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
