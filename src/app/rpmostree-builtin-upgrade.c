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
#include "rpmostree-dbus-helpers.h"

#include "libgsystem.h"
#include <libglnx.h>

static char *opt_sysroot = "/";
static char *opt_osname;
static gboolean opt_reboot;
static gboolean opt_allow_downgrade;
static gboolean opt_check_diff;
static gboolean opt_force_peer;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after an upgrade is prepared", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { "check-diff", 0, 0, G_OPTION_ARG_NONE, &opt_check_diff, "Check for upgrades and print package diff only", NULL },
  { "peer", 0, 0, G_OPTION_ARG_NONE, &opt_force_peer, "Force a peer to peer connection instead of using the system message bus", NULL },
  { NULL }
};

static GVariant *
get_args_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert (&dict, "allow-downgrade", "b", opt_allow_downgrade);

  return g_variant_dict_end (&dict);
}

static void
default_changed_callback (GObject *object,
                          GParamSpec *pspec,
                          gpointer user_data)
{
  GVariant **value = user_data;
  g_object_get (object, pspec->name, value, NULL);
}

gboolean
rpmostree_builtin_upgrade (int             argc,
                           char          **argv,
                           GCancellable   *cancellable,
                           GError        **error)
{
  gboolean ret = FALSE;

  GOptionContext *context = g_option_context_new ("- Perform a system upgrade");
  glnx_unref_object GDBusConnection *connection = NULL;
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autofree char *transaction_object_path = NULL;
  g_autoptr(GVariant) default_deployment = NULL;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  if (!rpmostree_load_connection_and_sysroot (opt_sysroot,
                                              opt_force_peer,
                                              cancellable,
                                              &connection,
                                              &sysroot_proxy,
                                              error))
    goto out;

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname,
                                cancellable, &os_proxy, error))
    goto out;

  if (opt_check_diff)
    {
      if (!rpmostree_os_call_download_update_rpm_diff_sync (os_proxy,
                                                            &transaction_object_path,
                                                            cancellable,
                                                            error))
        goto out;
    }
  else
    {
      g_signal_connect (os_proxy, "notify::default-deployment",
                        G_CALLBACK (default_changed_callback),
                        &default_deployment);

      if (!rpmostree_os_call_upgrade_sync (os_proxy,
                                           get_args_variant (),
                                           &transaction_object_path,
                                           cancellable,
                                           error))
        goto out;
    }

  if (!rpmostree_transaction_get_response_sync (connection,
                                                transaction_object_path,
                                                cancellable,
                                                error))
    goto out;

  if (opt_check_diff)
    {
      /* yes, doing this without using dbus */
      gs_unref_object OstreeSysroot *sysroot = NULL;
      gs_unref_object OstreeRepo *repo = NULL;
      gs_unref_object GFile *rpmdbdir = NULL;
      gs_unref_object GFile *sysroot_path = NULL;
      g_autofree char *origin_description = NULL;

      _cleanup_rpmrev_ struct RpmRevisionData *rpmrev1 = NULL;
      _cleanup_rpmrev_ struct RpmRevisionData *rpmrev2 = NULL;

      gs_free char *ref = NULL; /* location of this rev */
      gs_free char *remote = NULL;

      if (!rpmostree_os_get_has_cached_update_rpm_diff (os_proxy))
        goto out;

      sysroot_path = g_file_new_for_path (opt_sysroot);
      sysroot = ostree_sysroot_new (sysroot_path);

      if (!ostree_sysroot_load (sysroot, cancellable, error))
        goto out;

      if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
        goto out;

      origin_description = rpmostree_os_dup_upgrade_origin (os_proxy);
      if (!ostree_parse_refspec (origin_description, &remote, &ref, error))
         goto out;

      if (rpmReadConfigFiles (NULL, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "rpm failed to init: %s", rpmlogMessage ());
          goto out;
        }

      if (!(rpmrev1 = rpmrev_new (repo,
                                  ostree_deployment_get_csum (ostree_sysroot_get_booted_deployment (sysroot)),
                                  NULL, cancellable, error)))
        goto out;

      if (!(rpmrev2 = rpmrev_new (repo, ref,
                                  NULL, cancellable, error)))
        goto out;

      rpmhdrs_diff_prnt_diff (rpmhdrs_diff (rpmrev_get_headers (rpmrev1),
                                            rpmrev_get_headers (rpmrev2)));
    }
  else
    {
      /* nothing changed */
      if (default_deployment == NULL)
        {
          goto out;
        }
      if (opt_reboot)
        {
          gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                         cancellable, error,
                                         "systemctl", "reboot", NULL);
        }
      else
        {
          if (!rpmostree_print_treepkg_diff_from_sysroot_path (opt_sysroot,
                                                               cancellable,
                                                               error))
            goto out;

          g_print ("Run \"systemctl reboot\" to start a reboot\n");
        }
    }

  ret = TRUE;

out:
  /* Does nothing if using the message bus. */
  rpmostree_cleanup_peer ();

  return ret;
}
