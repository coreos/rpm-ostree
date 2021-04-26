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
#include "rpmostree-clientlib.h"
#include "rpmostree-editor.h"
#include "rpmostree-util.h"
#include <libglnx.h>

static gboolean opt_editor;
static gboolean opt_import_proc_cmdline;
static gboolean opt_reboot;
static char **opt_kernel_delete_strings;
static char **opt_kernel_append_strings;
static char **opt_kernel_delete_if_present_strings;
static char **opt_kernel_append_if_missing_strings;
static char **opt_kernel_replace_strings;
static char  *opt_osname;
static char  *opt_deploy_index;
static gboolean opt_lock_finalization;
static gboolean opt_unchanged_exit_77;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operation on provided OSNAME", "OSNAME" },
  { "deploy-index", 0, 0, G_OPTION_ARG_STRING, &opt_deploy_index, "Modify the kernel args from a specific deployment based on index. Index is in the form of a number (e.g. 0 means the first deployment in the list)", "INDEX"},
  { "reboot", 0, 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after operation is complete", NULL},
  { "append", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_append_strings, "Append kernel argument; useful with e.g. console= that can be used multiple times. empty value for an argument is allowed", "KEY=VALUE" },
  { "replace", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_replace_strings, "Replace existing kernel argument, the user is also able to replace an argument with KEY=VALUE if only one value exist for that argument ", "KEY=VALUE=NEWVALUE" },
  { "delete", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_delete_strings, "Delete a specific kernel argument key/val pair or an entire argument with a single key/value pair", "KEY=VALUE"},
  { "append-if-missing", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_append_if_missing_strings, "Like --append, but does nothing if the key is already present", "KEY=VALUE" },
  { "delete-if-present", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_delete_if_present_strings, "Like --delete, but does nothing if the key is already missing", "KEY=VALUE" },
  { "unchanged-exit-77", 0, 0, G_OPTION_ARG_NONE, &opt_unchanged_exit_77, "If no kernel args changed, exit 77", NULL },
  { "import-proc-cmdline", 0, 0, G_OPTION_ARG_NONE, &opt_import_proc_cmdline, "Instead of modifying old kernel arguments, we modify args from current /proc/cmdline (the booted deployment)", NULL },
  { "editor", 0, 0, G_OPTION_ARG_NONE, &opt_editor, "Use an editor to modify the kernel arguments", NULL },
  { "lock-finalization", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_lock_finalization, "Prevent automatic deployment finalization on shutdown", NULL },
  { NULL }
};

/*
 * This function shares a similar logic as the commit_editor
 * function in ostree code base. It takes old kernel arguments
 * and display to user through an editor. Then, then user
 * is able to modify those args.
 */
static gboolean
kernel_arg_handle_editor (const char     *input_kernel_arg,
                          const char    **out_kernel_arg,
                          GCancellable   *cancellable,
                          GError        **error)
{
  g_autofree char *chomped_input = g_strchomp (g_strdup (input_kernel_arg));

  g_autoptr(OstreeKernelArgs) temp_kargs = ostree_kernel_args_from_string (chomped_input);

  /* We check existance of ostree argument, if it
   * exists, we directly remove it. Also Note, since the current kernel
   * arguments are directly collected from the boot config, we expect that
   * there is only one value associated with ostree argument. Thus deleting
   * by one value should not error out here.
   */
  if (ostree_kernel_args_get_last_value (temp_kargs, "ostree") != NULL)
    {
      if (!ostree_kernel_args_delete (temp_kargs, "ostree", error))
        return FALSE;
    }
  g_autofree char* filtered_input = ostree_kernel_args_to_string (temp_kargs);

  g_autofree char *input_string = g_strdup_printf ("\n"
      "# Please enter the kernel arguments. Each kernel argument"
      "# should be in the form of key=value.\n"
      "# Lines starting with '#' will be ignored. Each key=value pair should be \n"
      "# separated by spaces, and multiple value associated with one key is allowed. \n"
      "# Also, please note that any changes to the ostree argument will not be \n"
      "# effective as they are usually regenerated when bootconfig changes. \n"
      "%s", filtered_input);

  /* Note: the repo here is NULL, as we don't really require it
   * for the edtior process
   */
  g_autofree char *out_editor_string = ot_editor_prompt (NULL, input_string,
                                                         cancellable, error);
  if (out_editor_string == NULL)
    return FALSE;

  /* We now process the editor output, we ignore empty lines and lines
   * starting with '#'
   */
  g_auto(GStrv) lines = g_strsplit (out_editor_string, "\n", -1);
  guint num_lines = g_strv_length (lines);
  g_autoptr(GString) kernel_arg_buf = g_string_new ("");
  for (guint i = 0; i < num_lines; i++)
    {
      char *line = lines[i];
      /* Remove leading and trailing spaces */
      g_strstrip (line);

      /* Comments and blank lines are skipped */
      if (*line == '#')
        continue;
      if (*line == '\0')
        continue;
      g_string_append (kernel_arg_buf, line);

      /* We append a space to every line except the last
       * because there could be a case where user have
       * args that are in multiple lines.
       */
      g_string_append (kernel_arg_buf,  " ");
    }

  /* Transfer the ownership to a new string, so we can remove the trailing spaces */
  g_autofree char *kernel_args_str = g_string_free (util::move_nullify (kernel_arg_buf), FALSE);
  g_strchomp (kernel_args_str);

  g_autoptr(OstreeKernelArgs) input_kargs = ostree_kernel_args_from_string (kernel_args_str);

  /* We check again to see if the ostree argument got added by the user */
  if (ostree_kernel_args_get_last_value (input_kargs, "ostree") != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "You have an 'ostree' argument in your input, that is not going to be handled");
      return FALSE;
    }

  /* We do not allow empty kernel arg string */
  if (g_str_equal (kernel_args_str, ""))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "The kernel arguments can not be empty");
      return FALSE;
    }

  /* Notify the user that nothing has been changed */
  if (g_str_equal (filtered_input, kernel_args_str))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "The kernel arguments remained the same");
      return FALSE;
    }

  *out_kernel_arg = util::move_nullify (kernel_args_str);

  return TRUE;
}


gboolean
rpmostree_builtin_kargs (int            argc,
                         char         **argv,
                         RpmOstreeCommandInvocation *invocation,
                         GCancellable  *cancellable,
                         GError       **error)
{
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
                                       NULL,
                                       error))
    return FALSE;

   if (opt_editor && (opt_kernel_delete_strings ||
       opt_kernel_replace_strings ||  opt_kernel_append_strings ||
       opt_kernel_delete_if_present_strings || opt_kernel_append_if_missing_strings))
     {
      /* We want editor command to achieve all these functionalities
       * Thus erroring out ahead of time when these strings exist
       */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify --editor with --replace, --delete, --append, --delete-if-present or --append-if-missing");
      return FALSE;
    }

  if (opt_kernel_delete_strings && opt_kernel_replace_strings)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --delete and --replace");
      return FALSE;
    }
  if (opt_import_proc_cmdline && opt_deploy_index)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both --import-from-proc-cmdline and --deploy-index");
      return FALSE;
    }
  if (opt_import_proc_cmdline && opt_osname)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot specify both osname and --import-from-proc-cmdline");
      return FALSE;
    }
  if (!(opt_kernel_delete_strings) && !(opt_kernel_append_strings)
      && !(opt_kernel_replace_strings) && !(opt_editor) &&
      !(opt_kernel_delete_if_present_strings) && !(opt_kernel_append_if_missing_strings))
    display_kernel_args = TRUE;

  if (opt_reboot && display_kernel_args)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot reboot when kernel arguments not changed");
      return FALSE;
    }

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    return FALSE;

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
    return FALSE;

  /* We extract the existing kernel arguments from the boot configuration */
  const char *old_kernel_arg_string = NULL;
  if (!g_variant_lookup (boot_config, "options",
                         "&s", &old_kernel_arg_string))
    return FALSE;

  if (display_kernel_args)
    {
      g_print ("%s\n", old_kernel_arg_string);
      return TRUE;
    }

  g_autofree char *transaction_address = NULL;
  char *empty_strv[] = {NULL};

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
  g_variant_dict_insert (&dict, "lock-finalization", "b", opt_lock_finalization);
  if (opt_kernel_append_if_missing_strings && *opt_kernel_append_if_missing_strings)
    g_variant_dict_insert (&dict, "append-if-missing", "^as", opt_kernel_append_if_missing_strings);
  if (opt_kernel_delete_if_present_strings && *opt_kernel_delete_if_present_strings)
    g_variant_dict_insert (&dict, "delete-if-present", "^as", opt_kernel_delete_if_present_strings);
  g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));
  g_autoptr(GVariant) previous_deployment = rpmostree_os_dup_default_deployment (os_proxy);

  if (opt_editor)
    {
      /* We track the kernel arg instance before the editor */
      const char *sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);
      g_autoptr(GFile) sysroot_file = g_file_new_for_path (sysroot_path);
      g_autoptr(OstreeSysroot) before_sysroot = ostree_sysroot_new (sysroot_file);
      if (!ostree_sysroot_load (before_sysroot, cancellable, error))
        return FALSE;

      const char* current_kernel_arg_string = NULL;
      if (!kernel_arg_handle_editor (old_kernel_arg_string, &current_kernel_arg_string,
                                     cancellable, error))
        return FALSE;
      gboolean out_changed = FALSE;

      /* Here we load the sysroot again, if the sysroot
       * has been changed since then, we directly error
       * out.
       */
      if (!ostree_sysroot_load_if_changed (before_sysroot,
                                           &out_changed,
                                           cancellable,
                                           error))
        return FALSE;
      if (out_changed)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "Conflict: bootloader configuration changed. Saved kernel arguments: \n%s", current_kernel_arg_string);

          return FALSE;
        }
      /* We use the user defined kernel args as existing arguments here
       * and kept other strvs empty, because the existing kernel arguments
       * is already enough for the update
       */
      if (!rpmostree_os_call_kernel_args_sync (os_proxy,
                                               current_kernel_arg_string,
                                               (const char* const*) empty_strv,
                                               (const char* const*) empty_strv,
                                               (const char* const*) empty_strv,
                                               options,
                                               &transaction_address,
                                               cancellable,
                                               error))
        return FALSE;
    }
  else
    {
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
                                               old_kernel_arg_string,
                                               (const char* const*) opt_kernel_append_strings,
                                               (const char* const*) opt_kernel_replace_strings,
                                               (const char* const*) opt_kernel_delete_strings,
                                               options,
                                               &transaction_address,
                                               cancellable,
                                               error))
        return FALSE;
    }

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy,
                                                transaction_address,
                                                cancellable,
                                                error))
    return FALSE;

  if (opt_unchanged_exit_77)
    {
      if (!rpmostree_has_new_default_deployment (os_proxy, previous_deployment))
        {
          invocation->exit_code = RPM_OSTREE_EXIT_UNCHANGED;
          return TRUE;
        }
    }

  if (rpmostree_has_new_default_deployment (os_proxy, previous_deployment))
    g_print("Kernel arguments updated.\nRun \"systemctl reboot\" to start a reboot\n");

  return TRUE;
}
