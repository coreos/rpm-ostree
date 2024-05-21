/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "rpmostree-builtins.h"
#include "rpmostree-clientlib.h"
#include "rpmostree-container.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"

#include <libglnx.h>

#include <gio/gunixfdlist.h>

#include <set>

static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_dry_run;
static gboolean opt_apply_live;
static gboolean opt_idempotent;
static gchar **opt_install;
static gboolean opt_assumeyes;
static gboolean opt_remove_kernel;
static gchar **opt_uninstall;
static gboolean opt_cache_only;
static gboolean opt_download_only;
static gboolean opt_allow_inactive;
static gboolean opt_uninstall_all;
static gboolean opt_unchanged_exit_77;
static gboolean opt_lock_finalization;
static gboolean opt_force_replacefiles;
static char **opt_enable_repo;
static char **opt_disable_repo;
static char **opt_release_ver;

static GOptionEntry option_entries[]
    = { { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
        { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot,
          "Initiate a reboot after operation is complete", NULL },
        { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &opt_dry_run, "Exit after printing the transaction",
          NULL },
        { "assumeyes", 'y', 0, G_OPTION_ARG_NONE, &opt_assumeyes,
          "Auto-confirm interactive prompts for non-security questions", NULL },
        { "allow-inactive", 0, 0, G_OPTION_ARG_NONE, &opt_allow_inactive,
          "Allow inactive package requests", NULL },
        { "idempotent", 0, 0, G_OPTION_ARG_NONE, &opt_idempotent,
          "Do nothing if package already (un)installed", NULL },
        { "unchanged-exit-77", 0, 0, G_OPTION_ARG_NONE, &opt_unchanged_exit_77,
          "If no overlays were changed, exit 77", NULL },
        { "lock-finalization", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_lock_finalization,
          "Prevent automatic deployment finalization on shutdown", NULL },
        { "enablerepo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_repo,
          "Enable the repository based on the repo id. Is only supported in a container build.",
          NULL },
        { "disablerepo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_disable_repo,
          "Only disabling all (*) repositories is supported currently. Is only supported in a "
          "container build.",
          NULL },
        { "releasever", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_release_ver,
          "Set the releasever. Is only supported in a container build.", NULL },
        { NULL } };

static GOptionEntry uninstall_option_entry[]
    = { { "install", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_install, "Overlay additional package",
          "PKG" },
        { "all", 0, 0, G_OPTION_ARG_NONE, &opt_uninstall_all,
          "Remove all overlayed additional packages", NULL },
        { NULL } };

static GOptionEntry install_option_entry[]
    = { { "uninstall", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_uninstall,
          "Remove overlayed additional package", "PKG" },
        { "cache-only", 'C', 0, G_OPTION_ARG_NONE, &opt_cache_only,
          "Do not download latest ostree and RPM data", NULL },
        { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only,
          "Just download latest ostree and RPM data, don't deploy", NULL },
        { "apply-live", 'A', 0, G_OPTION_ARG_NONE, &opt_apply_live,
          "Apply changes to both pending deployment and running filesystem tree", NULL },
        { "remove-installed-kernel", 0, 0, G_OPTION_ARG_NONE, &opt_remove_kernel,
          "Remove the installed kernel packages (usually helpful to install a different kernel)",
          NULL },
        { "force-replacefiles", 0, 0, G_OPTION_ARG_NONE, &opt_force_replacefiles,
          "Allow package to replace files from other packages", NULL },
        { NULL } };

static gboolean
pkg_change (RpmOstreeCommandInvocation *invocation, RPMOSTreeSysroot *sysroot_proxy,
            gboolean print_interactive, const char *const *packages_to_add,
            const char *const *packages_to_remove, GCancellable *cancellable, GError **error)
{
  const char *const strv_empty[] = { NULL };
  const char *const *install_pkgs = strv_empty;
  const char *const *install_fileoverride_pkgs = strv_empty;
  const char *const *uninstall_pkgs = strv_empty;

  if (packages_to_add && opt_force_replacefiles)
    install_fileoverride_pkgs = packages_to_add;
  else if (packages_to_add && !opt_force_replacefiles)
    install_pkgs = packages_to_add;
  if (packages_to_remove)
    uninstall_pkgs = packages_to_remove;

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname, cancellable, &os_proxy, error))
    return FALSE;

  g_autoptr (GVariant) previous_deployment = rpmostree_os_dup_default_deployment (os_proxy);

  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "reboot", "b", opt_reboot);
  g_variant_dict_insert (&dict, "cache-only", "b", opt_cache_only);
  g_variant_dict_insert (&dict, "download-only", "b", opt_download_only);
  g_variant_dict_insert (&dict, "no-pull-base", "b", TRUE);
  // Note that we pass dry-run to the server if we're actually asked for dry-run by the user,
  // or when we're trying to do an interactive prompt.
  g_variant_dict_insert (&dict, "dry-run", "b", opt_dry_run || print_interactive);
  g_variant_dict_insert (&dict, "print-interactive", "b", print_interactive);
  g_variant_dict_insert (&dict, "allow-inactive", "b", opt_allow_inactive);
  g_variant_dict_insert (&dict, "no-layering", "b", opt_uninstall_all);
  g_variant_dict_insert (&dict, "idempotent-layering", "b", opt_idempotent);
  g_variant_dict_insert (&dict, "initiating-command-line", "s", invocation->command_line);
  g_variant_dict_insert (&dict, "lock-finalization", "b", opt_lock_finalization);
  if (opt_apply_live)
    g_variant_dict_insert (&dict, "apply-live", "b", opt_apply_live);
  g_autoptr (GVariant) options = g_variant_ref_sink (g_variant_dict_end (&dict));

  gboolean met_local_pkg = FALSE;
  for (const char *const *it = packages_to_add; it && *it; it++)
    met_local_pkg
        = met_local_pkg || g_str_has_suffix (*it, ".rpm") || g_str_has_prefix (*it, "file://");

  /* Use newer D-Bus API only if we have to. */
  g_autofree char *transaction_address = NULL;
  if (met_local_pkg || opt_apply_live || (install_fileoverride_pkgs && *install_fileoverride_pkgs))
    {
      if (!rpmostree_update_deployment (os_proxy, NULL, /* refspec */
                                        NULL,           /* revision */
                                        install_pkgs, install_fileoverride_pkgs, uninstall_pkgs,
                                        NULL, /* override replace */
                                        NULL, /* override remove */
                                        NULL, /* override reset */
                                        NULL, /* local_repo_remote */
                                        NULL, /* treefile */
                                        options, &transaction_address, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!rpmostree_os_call_pkg_change_sync (os_proxy, options, install_pkgs, uninstall_pkgs, NULL,
                                              &transaction_address, NULL, cancellable, error))
        return FALSE;
    }

  return rpmostree_transaction_client_run (invocation, sysroot_proxy, os_proxy, options,
                                           opt_unchanged_exit_77, transaction_address,
                                           previous_deployment, cancellable, error);
}

gboolean
rpmostree_builtin_install (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, install_option_entry, NULL);

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, invocation,
                                       cancellable, NULL, NULL, &sysroot_proxy, error))
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

  // Special handling for -A without -y (and without --dry-run).
  const bool unconfirmed_live = !opt_assumeyes && opt_apply_live;
  if (unconfirmed_live && !opt_dry_run)
    {
      // In this case, `rpm-ostree install -A foo` is being invoked from a script.
      // In older versions, we didn't have the -y/--assumeyes option, so for backwards
      // compatibility, continue to work - but print a warning so they adjust their
      // scripts.
      if (!isatty (0))
        {
          g_printerr ("notice: auto-inferring -y/--assumeyes when not run interactively; this will "
                      "change in the future\n");
          g_usleep (G_USEC_PER_SEC);
          opt_assumeyes = 1;
        }
      else
        {
          // In this case, `rpm-ostree install -A foo` is being run *not* from a script

          // Recurse - this will update the caches, do depsolve etc.
          if (!pkg_change (invocation, sysroot_proxy, TRUE, (const char *const *)argv,
                           (const char *const *)opt_uninstall, cancellable, error))
            return FALSE;
          CXX_TRY (rpmostreecxx::confirm_or_abort (), error);
          // Fall through, the operation is confirmed.
        }
    }

  CXX_TRY_VAR (is_ostree_container, rpmostreecxx::is_ostree_container (), error);
  if (is_ostree_container)
    {
      CXX_TRY_VAR (treefile, rpmostreecxx::treefile_new_empty (), error);
      // TODO: better API/cache for this
      g_autoptr (DnfContext) ctx = dnf_context_new ();
      auto basearch = dnf_context_get_base_arch (ctx);
      for (char **it = argv; it && *it; it++)
        {
          auto pkg = *it;
          CXX_TRY_VAR (fds, rpmostreecxx::client_handle_fd_argument (pkg, basearch, false), error);
          if (fds.size () > 0)
            {
              CXX_TRY_VAR (pkgs, rpmostreecxx::stage_container_rpm_raw_fds (fds), error);
              CXX_TRY (treefile->add_local_packages (pkgs, true), error);
            }
          else
            {
              rust::Vec v = { rust::String (pkg) };
              CXX_TRY (treefile->add_packages (v, true), error);
            }
        }
      for (char **it = opt_enable_repo; it && *it; it++)
        {
          auto repo = rust::Str (*it);
          treefile->enable_repo (repo);
        }
      for (char **it = opt_disable_repo; it && *it; it++)
        {
          auto repo = rust::Str (*it);
          treefile->disable_repo (repo);
        }
      for (char **it = opt_release_ver; it && *it; it++)
        {
          auto ver = rust::Str (*it);
          treefile->set_releasever (ver);
        }
      if (opt_remove_kernel)
        treefile->set_remove_kernel (true);
      return rpmostree_container_rebuild (*treefile, cancellable, error);
    }
  else
    {
      // This very much relates to https://github.com/coreos/rpm-ostree/issues/2326 etc.
      if (opt_enable_repo)
        return glnx_throw (error, "--enablerepo currently only works in a container build");
      if (opt_disable_repo)
        return glnx_throw (error, "--disablerepo currently only works in a container build");
    }

  return pkg_change (invocation, sysroot_proxy, FALSE, (const char *const *)argv,
                     (const char *const *)opt_uninstall, cancellable, error);
}

gboolean
rpmostree_builtin_uninstall (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                             GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");

  g_option_context_add_main_entries (context, uninstall_option_entry, NULL);

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, invocation,
                                       cancellable, NULL, NULL, &sysroot_proxy, error))
    return FALSE;

  if (argc < 2 && !opt_uninstall_all)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return FALSE;
    }

  /* shift to first pkgspec and ensure it's a proper strv (previous parsing
   * might have moved args around) */
  argv++;
  argc--;
  argv[argc] = NULL;

  CXX_TRY_VAR (is_ostree_container, rpmostreecxx::is_ostree_container (), error);
  if (is_ostree_container)
    {
      CXX_TRY_VAR (treefile, rpmostreecxx::treefile_new_empty (), error);
      treefile->add_packages_override_remove (util::rust_stringvec_from_strv (argv));
      return rpmostree_container_rebuild (*treefile, cancellable, error);
    }

  /* If we don't also have to install pkgs, perform uninstalls offline; users don't expect
   * the "auto-update" behaviour here. */
  if (!opt_install)
    opt_cache_only = TRUE;

  return pkg_change (invocation, sysroot_proxy, FALSE, (const char *const *)opt_install,
                     (const char *const *)argv, cancellable, error);
}

struct cstrless
{
  bool
  operator() (const gchar *a, const gchar *b) const
  {
    return strcmp (a, b) < 0;
  }
};

gboolean
rpmostree_builtin_search (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                          GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;

  context = g_option_context_new ("PACKAGE [PACKAGE...]");
  g_option_context_add_main_entries (context, install_option_entry, NULL);
  g_option_context_add_main_entries (context, uninstall_option_entry, NULL);

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, invocation,
                                       cancellable, NULL, NULL, &sysroot_proxy, error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "At least one PACKAGE must be specified", error);
      return FALSE;
    }

  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname, cancellable, &os_proxy, error))
    return FALSE;

  g_autoptr (GPtrArray) arg_names = g_ptr_array_new ();
  for (guint i = 1; i < argc; i++)
    {
      g_ptr_array_add (arg_names, (char *)argv[i]);
    }
  g_ptr_array_add (arg_names, NULL);

  g_autoptr (GVariant) out_packages = NULL;

  if (!rpmostree_os_call_search_sync (os_proxy, (const char *const *)arg_names->pdata,
                                      &out_packages, cancellable, error))
    return FALSE;

  g_autoptr (GVariantIter) iter1 = NULL;
  g_variant_get (out_packages, "aa{sv}", &iter1);

  g_autoptr (GVariantIter) iter2 = NULL;
  std::set<const gchar *, cstrless> query_set;

  while (g_variant_iter_loop (iter1, "a{sv}", &iter2))
    {
      const gchar *key;
      const gchar *name;
      const gchar *summary;
      const gchar *query;
      const gchar *match_group = "";

      g_autoptr (GVariant) value = NULL;

      while (g_variant_iter_loop (iter2, "{sv}", &key, &value))
        {
          if (strcmp (key, "key") == 0)
            g_variant_get (value, "s", &query);
          else if (strcmp (key, "name") == 0)
            g_variant_get (value, "s", &name);
          else if (strcmp (key, "summary") == 0)
            g_variant_get (value, "s", &summary);
        }
      // The server must have sent these
      g_assert (name);
      g_assert (query);
      g_assert (summary);

      if (!query_set.count (query))
        {
          query_set.insert (query);

          if (strcmp (query, "match_group_a") == 0)
            match_group = "Summary & Name";
          else if (strcmp (query, "match_group_b") == 0)
            match_group = "Name";
          else if (strcmp (query, "match_group_c") == 0)
            match_group = "Summary";

          g_print ("\n===== %s Matched =====\n", match_group);
        }

      g_print ("%s : %s\n", name, summary);
    }

  if (query_set.size () == 0)
    {
      g_print ("No matches found.\n");
    }

  return TRUE;
}
