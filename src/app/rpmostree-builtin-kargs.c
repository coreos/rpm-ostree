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

/* TODO uncomment this when editor functionality is done
 *  static gboolean opt_editor;
 */
static gboolean opt_import_proc_cmdline;
static gboolean opt_reboot;
static char **opt_kernel_delete_strings;
static char **opt_kernel_append_strings;
static char **opt_kernel_replace_strings;
static char  *opt_osname;
static char  *opt_deploy_index;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operation on provided OSNAME", "OSNAME" },
  { "deploy-index", 0, 0, G_OPTION_ARG_STRING, &opt_deploy_index, "Modify the kernel args from a specific deployment based on index. Index is in the form of a number (e.g 0 means the first deployment in the list). The order of deployments can be seen via rpm-ostree status", "INDEX"},
  { "reboot", 0, 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after kernel arguments are modified", NULL},
  { "append", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_append_strings, "Append kernel argument; useful with e.g. console= that can be used multiple times. empty value for an argument is allowed", "KEY=VALUE" },
  { "replace", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_replace_strings, "Replace existing kernel argument, the user is also able to replace an argument with KEY=VALUE if only one value exist for that argument ", "KEY=VALUE=NEWVALUE" },
  { "delete", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_delete_strings, "Delete a specific kernel argument key/val pair or an entire argument with a single key/value pair", "KEY=VALUE"},
  { "import-proc-cmdline", 0, 0, G_OPTION_ARG_NONE, &opt_import_proc_cmdline, "Instead of modifying old kernel arguments, we modify args from current /proc/cmdline (the booted deployment)", NULL },
  /* TODO: Add back the editor option once we decided how to handle the similar API from ostree
   * { "editor", 0, 0, G_OPTION_ARG_NONE, &opt_editor, "Pops up an editor for users to change arguments", NULL },
   */
  { NULL }
};

static GVariant *
get_kargs_option_variant (gboolean is_authorized_prior)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "authorized", "b", is_authorized_prior);

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
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  gboolean display_kernel_args = FALSE;
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

  /* TODO: uncomment these lines when finish implementing editor feature
   *
   * if (opt_editor && (opt_import_proc || opt_kernel_delete_strings ||
   *   opt_kernel_replace_strings ||  opt_kernel_append_strings))
   *  {
   *    We want editor command to achieve all the other functionalities
   *   Thus erroring out ahead of time when multiple options exist
   *   g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
   *                "Cannot specify --editor with other options");
   *   return EXIT_FAILURE;
   * }
   */

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
  if (opt_import_proc_cmdline && opt_deploy_index)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Canno specify both --import-from-proc-cmdline and --deployid");
      return EXIT_FAILURE;
    }
  if (opt_import_proc_cmdline && opt_osname)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both osname and --import-from-proc-cmdline");
      return EXIT_FAILURE;
    }
  if (!(opt_kernel_delete_strings) && !(opt_kernel_append_strings)
      && !(opt_kernel_replace_strings))
    display_kernel_args = TRUE;

  if (opt_reboot && display_kernel_args)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot reboot when kernel arguments not changed");
      return EXIT_FAILURE;
    }

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return EXIT_FAILURE;

  /* The proc cmdline is the kernel args from booted deployment
   * if this option is not specified, we will default to find the first
   * pending  deployment that matches the osname if there is one
   */
  gboolean is_pending = !opt_import_proc_cmdline;

  /* Here we still keep the index as a string,
   * so that we can tell whether user has
   * input a index option, then in the backend,
   * we will parse the string into a number if
   * the index option is there
   */
  const char* deploy_index_str = opt_deploy_index ?: "";
  g_autoptr(GVariant) boot_config = NULL;
  if (!rpmostree_os_call_get_deployment_boot_config_sync (os_proxy,
                                                          deploy_index_str,
                                                          is_pending,
                                                          &boot_config,
                                                          cancellable,
                                                          error))
    return EXIT_FAILURE;

  /* We extract the existing kernel arguments from the boot configuration */
  const char *existing_kernel_arg_string = NULL;
  if (!g_variant_lookup (boot_config, "options",
                         "&s", &existing_kernel_arg_string))
    return EXIT_FAILURE;

  if (display_kernel_args)
    {
      g_print("The kernel arguments are:\n%s\n",
              existing_kernel_arg_string);
      return EXIT_SUCCESS;
    }

  g_autofree char *transaction_address = NULL;
  char *empty_strv[] = {NULL};

  /* dbus does not allow NULL to mean the empty string array,
   * assign them to be empty string array here to prevent erroring out
   */
  if (!opt_kernel_replace_strings)
    opt_kernel_replace_strings = empty_strv;
  if (!opt_kernel_append_strings)
    opt_kernel_append_strings = empty_strv;
  if (!opt_kernel_delete_strings)
    opt_kernel_delete_strings = empty_strv;

  /* call the generearted dbus-function */
  if (!rpmostree_os_call_kernel_args_sync (os_proxy,
                                           existing_kernel_arg_string,
                                           (const char* const*) opt_kernel_append_strings,
                                           (const char* const*) opt_kernel_replace_strings,
                                           (const char* const*) opt_kernel_delete_strings,
                                           get_kargs_option_variant (TRUE),
                                           &transaction_address,
                                           cancellable,
                                           error))
    return EXIT_FAILURE;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return EXIT_FAILURE;

  g_print("Kernel arguments updated.\nRun \"systemctl reboot\" to start a reboot\n");

  return EXIT_SUCCESS;
}
