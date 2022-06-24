/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
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

#include "rpmostree-container.h"
#include "rpmostree-core.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-override-builtins.h"

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_cache_only;
static gboolean opt_dry_run;
static gboolean opt_reset_all;
static const char *const *opt_remove_pkgs;
static const char *const *opt_replace_pkgs;
static const char *const *install_pkgs;
static const char *const *uninstall_pkgs;
static const gchar *opt_from;
static gboolean opt_lock_finalization;
static gboolean opt_experimental;
static gboolean opt_freeze;

static GOptionEntry option_entries[]
    = { { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
        { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot,
          "Initiate a reboot after operation is complete", NULL },
        { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &opt_dry_run, "Exit after printing the transaction",
          NULL },
        { "lock-finalization", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_lock_finalization,
          "Prevent automatic deployment finalization on shutdown", NULL },
        { "cache-only", 'C', 0, G_OPTION_ARG_NONE, &opt_cache_only, "Only operate on cached data",
          NULL },
        { NULL } };

static GOptionEntry reset_option_entries[]
    = { { "all", 'a', 0, G_OPTION_ARG_NONE, &opt_reset_all, "Reset all active overrides", NULL },
        { NULL } };

static GOptionEntry replace_option_entries[]
    = { { "remove", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_remove_pkgs, "Remove a package", "PKG" },
        { "experimental", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_experimental,
          "Allow experimental options such as --from or --freeze", NULL },
        { "freeze", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_freeze,
          "Pin packages to versions found", NULL },
        { "from", 'r', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &opt_from, "Source of override",
          "KIND=NAME" },
        { NULL } };

static GOptionEntry remove_option_entries[] = { { "replace", 0, 0, G_OPTION_ARG_STRING_ARRAY,
                                                  &opt_replace_pkgs, "Replace a package", "RPM" },
                                                { NULL } };

static gboolean
sort_replacements (RPMOSTreeOSExperimental *osexperimental_proxy,
                   const char *const *replacement_args, char ***out_local, char ***out_remote,
                   GUnixFDList **out_fd_list, GCancellable *cancellable, GError **error)
{
  g_autoptr (GPtrArray) local = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) freeze = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) remote = g_ptr_array_new_with_free_func (g_free);
  for (const char *const *it = replacement_args; it && *it; it++)
    {
      auto arg = *it;
      if (rpmostreecxx::is_http_arg (arg) || rpmostreecxx::is_rpm_arg (arg))
        {
          g_ptr_array_add (local, (gpointer)g_strdup (arg));
        }
      // if the pkg is a valid nevra, then treat it as frozen
      else if (opt_freeze || rpmostree_is_valid_nevra (arg))
        {
          g_ptr_array_add (freeze, (gpointer)g_strdup (arg));
        }
      else
        {
          g_ptr_array_add (remote, (gpointer)g_strdup (arg));
        }
    }

  g_autoptr (GUnixFDList) fd_list = NULL;
  if (freeze->len > 0)
    {
      g_ptr_array_add (freeze, NULL);

      if (osexperimental_proxy)
        {
          /* this is really just a D-Bus wrapper around `rpmostree_find_and_download_packages` */
          if (!rpmostree_osexperimental_call_download_packages_sync (
                  osexperimental_proxy, (const char *const *)freeze->pdata, opt_from, NULL,
                  &fd_list, cancellable, error))
            return FALSE;
        }
      else
        {
          if (!rpmostree_find_and_download_packages ((const char *const *)freeze->pdata, opt_from,
                                                     "/", NULL, &fd_list, cancellable, error))
            return FALSE;
        }

      int nfds;
      const int *fds = g_unix_fd_list_peek_fds (fd_list, &nfds);
      for (guint i = 0; i < nfds; i++)
        g_ptr_array_add (local, g_strdup_printf ("file:///proc/self/fd/%d", fds[i]));
    }

  if (local->len > 0)
    {
      g_ptr_array_add (local, NULL);
      *out_local = (char **)g_ptr_array_free (util::move_nullify (local), FALSE);
    }
  else
    *out_local = NULL;

  if (remote->len > 0)
    {
      g_ptr_array_add (remote, NULL);
      *out_remote = (char **)g_ptr_array_free (util::move_nullify (remote), FALSE);
    }
  else
    *out_remote = NULL;

  *out_fd_list = util::move_nullify (fd_list);
  return TRUE;
}

static gboolean
handle_override (RPMOSTreeSysroot *sysroot_proxy, RpmOstreeCommandInvocation *invocation,
                 const char *const *override_remove, const char *const *override_replace,
                 const char *const *override_reset, GCancellable *cancellable, GError **error)
{
  CXX_TRY_VAR (is_ostree_container, rpmostreecxx::is_ostree_container (), error);

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeOSExperimental *osexperimental_proxy = NULL;
  if (!is_ostree_container)
    {
      if (!rpmostree_load_os_proxies (sysroot_proxy, opt_osname, cancellable, &os_proxy,
                                      &osexperimental_proxy, error))
        return FALSE;
    }

  if (!opt_experimental && (opt_freeze || opt_from))
    return glnx_throw (error, "Must specify --experimental to use --freeze or --from");
  if (is_ostree_container && override_reset && *override_reset)
    return glnx_throw (error, "Resetting overrides is not supported in container mode");

  CXX_TRY_VAR (treefile, rpmostreecxx::treefile_new_empty (), error);
  g_autoptr (GUnixFDList) fd_list = NULL; /* this is just used to own the fds */

  /* By default, we assume all the args are local overrides. If --from is used, we have to sort them
   * first to handle --freeze and remote overrides. */
  g_auto (GStrv) override_replace_local = NULL;
  if (override_replace && opt_from)
    {
      g_auto (GStrv) override_replace_remote = NULL;
      if (!sort_replacements (osexperimental_proxy, override_replace, &override_replace_local,
                              &override_replace_remote, &fd_list, cancellable, error))
        return FALSE;

      if (override_replace_remote)
        {
          CXX_TRY_VAR (parsed_source, rpmostreecxx::parse_override_source (opt_from), error);
          rpmostreecxx::OverrideReplacement replacement = {
            parsed_source.name,
            parsed_source.kind,
            util::rust_stringvec_from_strv (override_replace_remote),
          };
          treefile->add_packages_override_replace (replacement);
        }

      override_replace = override_replace_local;
    }

  if (is_ostree_container)
    {
      for (const char *const *it = override_replace; it && *it; it++)
        {
          auto arg = *it;
          // TODO: better API/cache for this
          g_autoptr (DnfContext) ctx = dnf_context_new ();
          auto basearch = dnf_context_get_base_arch (ctx);
          CXX_TRY_VAR (fds, rpmostreecxx::client_handle_fd_argument (arg, basearch, true), error);
          g_assert_cmpint (fds.size (), >, 0);
          CXX_TRY_VAR (pkgs, rpmostreecxx::stage_container_rpm_raw_fds (fds), error);
          treefile->add_packages_override_replace_local (pkgs);
        }
      treefile->add_packages_override_remove (util::rust_stringvec_from_strv (override_remove));
      return rpmostree_container_rebuild (*treefile, cancellable, error);
    }

  /* Perform uninstalls offline; users don't expect the "auto-update" behaviour here. But
   * note we might still need to fetch pkgs in the local replacement case (e.g. the
   * replacing pkg has an additional out-of-tree dep). */
  const gboolean cache_only = opt_cache_only || (override_replace == NULL && install_pkgs == NULL);

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "cache-only", "b", cache_only);
  g_variant_dict_insert (&dict, "no-pull-base", "b", TRUE);
  g_variant_dict_insert (&dict, "dry-run", "b", opt_dry_run);
  g_variant_dict_insert (&dict, "no-overrides", "b", opt_reset_all);
  g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
  g_variant_dict_insert (&dict, "lock-finalization", "b", opt_lock_finalization);
  g_autoptr (GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

  g_autoptr (GVariant) previous_deployment = rpmostree_os_dup_default_deployment (os_proxy);

  rust::String treefile_s;
  const char *treefile_c = NULL;
  if (treefile->has_any_packages ())
    {
      treefile_s = treefile->get_json_string ();
      treefile_c = treefile_s.c_str ();
    }

  g_autofree char *transaction_address = NULL;
  if (!rpmostree_update_deployment (os_proxy, NULL,     /* set-refspec */
                                    NULL,               /* set-revision */
                                    install_pkgs, NULL, /* install_fileoverride_pkgs */
                                    uninstall_pkgs, override_replace, override_remove,
                                    override_reset, NULL, treefile_c, options, &transaction_address,
                                    cancellable, error))
    return FALSE;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy, transaction_address, cancellable,
                                                error))
    return FALSE;

  if (opt_dry_run)
    {
      g_print ("Exiting because of '--dry-run' option\n");
    }
  else if (!opt_reboot)
    {
      /* only print diff if a new deployment was laid down (e.g. reset --all may not) */
      if (!rpmostree_has_new_default_deployment (os_proxy, previous_deployment))
        return TRUE;

      const char *sysroot_path = rpmostree_sysroot_get_path (sysroot_proxy);
      ROSCXX_TRY (print_treepkg_diff_from_sysroot_path (rust::Str (sysroot_path),
                                                        RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE,
                                                        0, cancellable),
                  error);

      if (override_replace || override_remove)
        g_print ("Use \"rpm-ostree override reset\" to undo overrides\n");

      g_print ("Run \"systemctl reboot\" to start a reboot\n");
    }

  return TRUE;
}

gboolean
rpmostree_override_builtin_replace (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                                    GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, replace_option_entries, NULL);

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, invocation,
                                       cancellable, &install_pkgs, &uninstall_pkgs, &sysroot_proxy,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return FALSE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++;
  argc--;
  argv[argc] = NULL;

  return handle_override (sysroot_proxy, invocation, opt_remove_pkgs, (const char *const *)argv,
                          NULL, cancellable, error);
}

gboolean
rpmostree_override_builtin_remove (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                                   GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, remove_option_entries, NULL);

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, invocation,
                                       cancellable, &install_pkgs, &uninstall_pkgs, &sysroot_proxy,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return FALSE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++;
  argc--;
  argv[argc] = NULL;

  return handle_override (sysroot_proxy, invocation, (const char *const *)argv, opt_replace_pkgs,
                          NULL, cancellable, error);
}

gboolean
rpmostree_override_builtin_reset (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                                  GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, reset_option_entries, NULL);

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, invocation,
                                       cancellable, &install_pkgs, &uninstall_pkgs, &sysroot_proxy,
                                       error))
    return FALSE;

  if (argc < 2 && !opt_reset_all)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return FALSE;
    }
  else if (opt_reset_all && argc >= 2)
    {
      rpmostree_usage_error (context, "Cannot specify PACKAGEs with --all", error);
      return FALSE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++;
  argc--;
  argv[argc] = NULL;

  return handle_override (sysroot_proxy, invocation, NULL, NULL, (const char *const *)argv,
                          cancellable, error);
}
