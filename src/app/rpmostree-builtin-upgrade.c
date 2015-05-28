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
#include <gio/gio.h>

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpm-ostreed-generated.h"

#include "libgsystem.h"

static char *opt_sysroot = "/";
static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_allow_downgrade;
static gboolean opt_check_diff;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after an upgrade is prepared", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { "check-diff", 0, 0, G_OPTION_ARG_NONE, &opt_check_diff, "Check for upgrades and print package diff only", NULL },
  { NULL }
};

#define DBUS_NAME "org.projectatomic.rpmostree1"
#define BASE_DBUS_PATH "/org/projectatomic/rpmostree1"

static void
gpg_verify_result_cb (OstreeRepo *repo,
                      const char *checksum,
                      OstreeGpgVerifyResult *result,
                      GSConsole *console)
{
  /* Temporarily place the GSConsole stream (which is just stdout)
   * back in normal mode before printing GPG verification results. */
  gs_console_end_status_line (console, NULL, NULL);

  g_print ("\n");
  rpmostree_print_gpg_verify_result (result);
}

static gboolean
rpmostree_builtin_upgrade_check_diff (GFile *sysroot_path,
                                      GCancellable *cancellable,
                                      GError **error)
{
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  glnx_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  g_autofree char *origin_description = NULL;
  GSConsole *console = NULL;
  gulong signal_handler_id = 0;
  gboolean changed = FALSE;
  gboolean ret = FALSE;

  sysroot = ostree_sysroot_new (sysroot_path);
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  upgrader = ostree_sysroot_upgrader_new_for_os (sysroot, opt_osname,
                                                 cancellable, error);
  if (!upgrader)
    goto out;

  origin_description = ostree_sysroot_upgrader_get_origin_description (upgrader);
  if (origin_description != NULL)
    g_print ("Updating from: %s\n", origin_description);

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  console = gs_console_get ();
  if (console != NULL)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
      signal_handler_id = g_signal_connect (repo, "gpg-verify-result",
                                            G_CALLBACK (gpg_verify_result_cb),
                                            console);

    }

  if (!ostree_sysroot_upgrader_pull_one_dir (upgrader, "/usr/share/rpm",
                                             0, 0, progress, &changed,
                                             cancellable, error))
    goto out;

  if (console != NULL)
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
      g_print ("No upgrade available.\n");
    }
  else
    {
      glnx_unref_object GFile *rpmdbdir = NULL;
      _cleanup_rpmrev_ struct RpmRevisionData *rpmrev1 = NULL;
      _cleanup_rpmrev_ struct RpmRevisionData *rpmrev2 = NULL;

      gs_free char *tmpd = g_mkdtemp (g_strdup ("/tmp/rpm-ostree.XXXXXX"));

      gs_free char *ref = NULL; // location of this rev
      gs_free char *remote = NULL;

      if (!ostree_parse_refspec (origin_description, &remote, &ref, error))
         goto out;

      if (rpmReadConfigFiles (NULL, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "rpm failed to init: %s", rpmlogMessage());
          goto out;
        }

      rpmdbdir = g_file_new_for_path (tmpd);

      if (!(rpmrev1 = rpmrev_new (repo, rpmdbdir,
                                  ostree_deployment_get_csum (ostree_sysroot_get_booted_deployment (sysroot)),
                                  NULL, cancellable, error)))
        goto out;

      if (!(rpmrev2 = rpmrev_new (repo, rpmdbdir, ref,
                                  NULL, cancellable, error)))
        goto out;

      rpmhdrs_diff_prnt_diff (rpmrev1->root, rpmrev2->root,
                              rpmhdrs_diff (rpmrev1->rpmdb, rpmrev2->rpmdb));
    }

  ret = TRUE;

out:
  if (console != NULL)
    (void) gs_console_end_status_line (console, NULL, NULL);

  if (signal_handler_id > 0)
    g_signal_handler_disconnect (repo, signal_handler_id);

  return ret;
}

static gboolean
rpmostree_builtin_upgrade_for_sysroot (GFile *sysroot_path,
                                       GCancellable *cancellable,
                                       GError **error)
{
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  glnx_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  g_autofree char *origin_description = NULL;
  GSConsole *console = NULL;
  OstreeSysrootUpgraderPullFlags upgrader_pull_flags = 0;
  gulong signal_handler_id = 0;
  gboolean changed = FALSE;
  gboolean ret = FALSE;

  sysroot = ostree_sysroot_new (sysroot_path);
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  upgrader = ostree_sysroot_upgrader_new_for_os (sysroot, opt_osname,
                                                 cancellable, error);
  if (!upgrader)
    goto out;

  origin_description = ostree_sysroot_upgrader_get_origin_description (upgrader);
  if (origin_description != NULL)
    g_print ("Updating from: %s\n", origin_description);

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  console = gs_console_get ();
  if (console != NULL)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
      signal_handler_id = g_signal_connect (repo, "gpg-verify-result",
                                            G_CALLBACK (gpg_verify_result_cb),
                                            console);

    }

  if (opt_allow_downgrade)
    upgrader_pull_flags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

  if (!ostree_sysroot_upgrader_pull (upgrader, 0, upgrader_pull_flags,
                                     progress, &changed,
                                     cancellable, error))
    goto out;

  if (console != NULL)
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
      g_print ("No upgrade available.\n");
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
          if (!rpmostree_print_treepkg_diff (sysroot, cancellable, error))
            goto out;

          g_print ("Upgrade prepared for next boot; run \"systemctl reboot\" to start a reboot\n");
        }
    }

  ret = TRUE;

out:
  if (console != NULL)
    (void) gs_console_end_status_line (console, NULL, NULL);

  if (signal_handler_id > 0)
    g_signal_handler_disconnect (repo, signal_handler_id);

  return ret;
}

static gboolean
rpmostree_builtin_upgrade_system (GCancellable *cancellable,
                                  GError **error)
{
  glnx_unref_object GDBusConnection *connection = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeTransaction *transaction_proxy = NULL;
  g_auto(GVariantDict) options;
  g_autofree char *os_object_path = NULL;
  g_autofree char *transaction_object_path = NULL;
  gboolean ret = FALSE;

  g_variant_dict_init (&options, NULL);

  g_variant_dict_insert (&options,
                         "allow-downgrade",
                         "b", opt_allow_downgrade);

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, error);
  if (connection == NULL)
    goto out;

  sysroot_proxy = rpmostree_sysroot_proxy_new_sync (connection,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    DBUS_NAME,
                                                    BASE_DBUS_PATH,
                                                    cancellable,
                                                    error);
  if (sysroot_proxy == NULL)
    goto out;

  if (opt_osname == NULL)
    {
      os_object_path = rpmostree_sysroot_dup_booted (sysroot_proxy);
    }
  else
    {
      if (!rpmostree_sysroot_call_get_os_sync (sysroot_proxy,
                                               opt_osname,
                                               &os_object_path,
                                               cancellable,
                                               error))
        goto out;
    }

  os_proxy = rpmostree_os_proxy_new_sync (connection,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          DBUS_NAME,
                                          os_object_path,
                                          cancellable,
                                          error);
  if (os_proxy != NULL)
    goto out;

  if (!rpmostree_os_call_upgrade_sync (os_proxy,
                                       g_variant_dict_end (&options),
                                       &transaction_object_path,
                                       cancellable,
                                       error))
    goto out;

  /* XXX I worry this part might be racy.  If the transaction completes
   *     before we connect to the interface, we may miss it entirely and
   *     not know whether the transaction succeeded and end up reporting
   *     a bogus D-Bus error.
   *
   *     One pattern I've used in the past is to add a Start() method to
   *     the transaction interface to call once the client is set up, but
   *     that complicates the server-side considerably: have to abort the
   *     transaction if the client dies or a timer expires before Start()
   *     is called.
   *
   *     Reference code for this pattern:
   *     https://git.gnome.org/browse/evolution-data-server/tree/libebackend/e-authentication-mediator.c?h=evolution-data-server-3-12
   */

  transaction_proxy = rpmostree_transaction_proxy_new_sync (connection,
                                                            G_DBUS_PROXY_FLAGS_NONE,
                                                            DBUS_NAME,
                                                            transaction_object_path,
                                                            cancellable,
                                                            error);
  if (transaction_proxy != NULL)
    goto out;

  /* FIXME Monitor the transaction, report progress, etc. */

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
  g_autoptr(GFile) sysroot_path = NULL;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  /* XXX Like g_file_new_for_commandline_arg() but no URIs. */
  if (g_path_is_absolute (opt_sysroot))
    {
      sysroot_path = g_file_new_for_path (opt_sysroot);
    }
  else
    {
      g_autofree char *current_dir = NULL;
      g_autofree char *filename = NULL;

      current_dir = g_get_current_dir ();
      filename = g_build_filename (current_dir, opt_sysroot, NULL);
      sysroot_path = g_file_new_for_path (filename);
    }

  if (opt_check_diff)
    {
      if (!rpmostree_builtin_upgrade_check_diff (sysroot_path,
                                                 cancellable, error))
        goto out;
    }
  else if (g_file_has_parent (sysroot_path, NULL))
    {
      if (!rpmostree_builtin_upgrade_for_sysroot (sysroot_path,
                                                  cancellable, error))
        goto out;
    }
  else
    {
      if (!rpmostree_builtin_upgrade_system (cancellable, error))
        goto out;
    }

  ret = TRUE;

 out:
  return ret;
}
