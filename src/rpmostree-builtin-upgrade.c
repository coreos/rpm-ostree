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

#include <string.h>
#include <glib-unix.h>

#include "rpmostree-builtins.h"

#include <hawkey/packagelist.h>
#include <hawkey/query.h>
#include <hawkey/sack.h>
#include <hawkey/stringarray.h>
#include <hawkey/goal.h>
#include <hawkey/version.h>
#include <hawkey/util.h>
#include "hif-utils.h"

#include "libgsystem.h"

static gboolean opt_reboot;

static GOptionEntry option_entries[] = {
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after an upgrade is prepared", NULL },
  { NULL }
};

static void
pull_progress (OstreeAsyncProgress       *progress,
               gpointer                   user_data)
{
  GSConsole *console = user_data;
  GString *buf;
  gs_free char *status = NULL;
  guint outstanding_fetches;
  guint outstanding_writes;
  guint n_scanned_metadata;

  if (!console)
    return;

  buf = g_string_new ("");

  status = ostree_async_progress_get_status (progress);
  outstanding_fetches = ostree_async_progress_get_uint (progress, "outstanding-fetches");
  outstanding_writes = ostree_async_progress_get_uint (progress, "outstanding-writes");
  n_scanned_metadata = ostree_async_progress_get_uint (progress, "scanned-metadata");
  if (status)
    {
      g_string_append (buf, status);
    }
  else if (outstanding_fetches)
    {
      guint64 bytes_transferred = ostree_async_progress_get_uint64 (progress, "bytes-transferred");
      guint fetched = ostree_async_progress_get_uint (progress, "fetched");
      guint requested = ostree_async_progress_get_uint (progress, "requested");
      gs_free char *formatted_bytes_transferred =
        g_format_size_full (bytes_transferred, 0);

      g_string_append_printf (buf, "Receiving objects: %u%% (%u/%u) %s",
                              (guint)((((double)fetched) / requested) * 100),
                              fetched, requested, formatted_bytes_transferred);
    }
  else if (outstanding_writes)
    {
      g_string_append_printf (buf, "Writing objects: %u", outstanding_writes);
    }
  else
    {
      g_string_append_printf (buf, "Scanning metadata: %u", n_scanned_metadata);
    }

  gs_console_begin_status_line (console, buf->str, NULL, NULL);
  
  g_string_free (buf, TRUE);
}

/* Todo: move this to libgsystem */
#define DEFINE_TRIVIAL_CLEANUP_FUNC(type, func)                 \
        static inline void func##p(type *p) {                   \
                if (*p)                                         \
                        func(*p);                               \
        }                                                       \
        struct __useless_struct_to_allow_trailing_semicolon__

DEFINE_TRIVIAL_CLEANUP_FUNC(HySack, hy_sack_free);
DEFINE_TRIVIAL_CLEANUP_FUNC(HyQuery, hy_query_free);
DEFINE_TRIVIAL_CLEANUP_FUNC(HyPackageList, hy_packagelist_free);

#define _cleanup_hysack_ __attribute__((cleanup(hy_sack_freep)))
#define _cleanup_hyquery_ __attribute__((cleanup(hy_query_freep)))
#define _cleanup_hypackagelist_ __attribute__((cleanup(hy_packagelist_freep)))

static gboolean
get_pkglist_for_root (GFile            *root,
                      HySack           *out_sack,
                      HyPackageList    *out_pkglist,
                      GCancellable     *cancellable,
                      GError          **error)
{
  gboolean ret = FALSE;
  int rc;
  _cleanup_hysack_ HySack sack = NULL;
  _cleanup_hyquery_ HyQuery query = NULL;
  _cleanup_hypackagelist_ HyPackageList pkglist = NULL;

  sack = hy_sack_create (NULL, NULL, gs_file_get_path_cached (root), HY_MAKE_CACHE_DIR);
  if (sack == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create sack cache");
      goto out;
    }

  rc = hy_sack_load_system_repo (sack, NULL, HY_BUILD_CACHE);
  if (!hif_rc_to_gerror (rc, error))
    {
      g_prefix_error (error, "Failed to load system repo: ");
      goto out;
    }
  query = hy_query_create (sack);
  hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  pkglist = hy_query_run (query);

  ret = TRUE;
  gs_transfer_out_value (out_sack, &sack);
  gs_transfer_out_value (out_pkglist, &pkglist);
 out:
  return ret;
}

static gboolean
print_rpmdb_diff (GFile          *oldroot,
                  GFile          *newroot,
                  GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  _cleanup_hysack_ HySack old_sack = NULL;
  _cleanup_hypackagelist_ HyPackageList old_pkglist = NULL;
  _cleanup_hysack_ HySack new_sack = NULL;
  _cleanup_hypackagelist_ HyPackageList new_pkglist = NULL;
  guint i;
  HyPackage pkg;
  gboolean printed_header = FALSE;

  if (!get_pkglist_for_root (oldroot, &old_sack, &old_pkglist,
                             cancellable, error))
    goto out;

  if (!get_pkglist_for_root (newroot, &new_sack, &new_pkglist,
                             cancellable, error))
    goto out;
  
  printed_header = FALSE;
  FOR_PACKAGELIST(pkg, old_pkglist, i)
    {
      _cleanup_hyquery_ HyQuery query = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      
      query = hy_query_create (new_sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, hy_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_EVR, HY_NEQ, hy_package_get_evr (pkg));
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      pkglist = hy_query_run (query);
      if (hy_packagelist_count (pkglist) > 0)
        {
          gs_free char *nevra = hy_package_get_nevra (pkg);
          if (!printed_header)
            {
              g_print ("Changed:\n");
              printed_header = TRUE;
            }
          g_print ("  %s\n", nevra);
        }
    }

  printed_header = FALSE;
  FOR_PACKAGELIST(pkg, old_pkglist, i)
    {
      _cleanup_hyquery_ HyQuery query = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      
      query = hy_query_create (new_sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, hy_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      pkglist = hy_query_run (query);
      if (hy_packagelist_count (pkglist) == 0)
        {
          gs_free char *nevra = hy_package_get_nevra (pkg);
          if (!printed_header)
            {
              g_print ("Removed:\n");
              printed_header = TRUE;
            }
          g_print ("  %s\n", nevra);
        }
    }

  printed_header = FALSE;
  FOR_PACKAGELIST(pkg, new_pkglist, i)
    {
      _cleanup_hyquery_ HyQuery query = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      
      query = hy_query_create (old_sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, hy_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      pkglist = hy_query_run (query);
      if (hy_packagelist_count (pkglist) == 0)
        {
          gs_free char *nevra = hy_package_get_nevra (pkg);
          if (!printed_header)
            {
              g_print ("Added:\n");
              printed_header = TRUE;
            }
          g_print ("  %s\n", nevra);
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_builtin_upgrade (int             argc,
                           char          **argv,
                           GCancellable   *cancellable,
                           GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("- Perform a system upgrade");
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gs_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  gs_unref_object OstreeAsyncProgress *progress = NULL;
  GSConsole *console;
  gboolean changed;
  gs_free char *origin_description = NULL;
  
  g_option_context_add_main_entries (context, option_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  sysroot = ostree_sysroot_new_default ();
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  upgrader = ostree_sysroot_upgrader_new (sysroot, cancellable, error);
  if (!upgrader)
    goto out;

  origin_description = ostree_sysroot_upgrader_get_origin_description (upgrader);
  if (origin_description)
    g_print ("Updating from: %s\n", origin_description);

  console = gs_console_get ();
  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (pull_progress, console);
    }

  if (!ostree_sysroot_upgrader_pull (upgrader, 0, 0, progress, &changed,
                                     cancellable, error))
    goto out;

  if (console)
    {
      if (!gs_console_end_status_line (console, cancellable, error))
        {
          console = NULL;
          goto out;
        }
      console = NULL;
    }

  if (!changed)
    {
      g_print ("No updates available.\n");
    }
  else
    {
      if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
        goto out;

      if (opt_reboot)
        gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                       cancellable, error,
                                       "systemctl", "reboot", NULL);
      else
        {
          gs_unref_object GFile *current_root = g_file_new_for_path ("/");
          gs_unref_object GFile *new_root = NULL;
          gs_unref_ptrarray GPtrArray *new_deployments = 
            ostree_sysroot_get_deployments (sysroot);
          OstreeDeployment *new_deployment;

          g_assert (new_deployments->len > 1);
          new_deployment = new_deployments->pdata[0];
          new_root = ostree_sysroot_get_deployment_directory (sysroot, new_deployment);

          if (!print_rpmdb_diff (current_root, new_root,
                                 cancellable, error))
            goto out;

          g_print ("Updates prepared for next boot; run \"systemctl reboot\" to start a reboot\n");
        }
    }
  
  ret = TRUE;
 out:
  if (console)
    (void) gs_console_end_status_line (console, NULL, NULL);

  return ret;
}
