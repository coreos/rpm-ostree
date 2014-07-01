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

#include <libsoup/soup.h>

#include "rpmostree-builtins.h"
#include "rpmostree-treepkgdiff.h"
#include "rpmostree-pull-progress.h"

#include "libgsystem.h"

#include "ostree-sysroot-upgrader.h"

static gboolean opt_reboot;
static gboolean opt_allow_downgrade;
static gboolean opt_check_only;

static GOptionEntry option_entries[] = {
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after an upgrade is prepared", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { "check-only", 'c', 0, G_OPTION_ARG_NONE, &opt_check_only, "Check for upgrades and print true/false only", NULL },
  { NULL }
};

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
  GSConsole *console = NULL;
  gboolean changed;
  OstreeSysrootUpgraderPullFlags upgraderpullflags = 0;
  gs_free char *origin_description = NULL;
  guint i;

  GKeyFile *config = NULL; // config file, holds URL
  gs_free char *url = NULL; // url from config file
  gs_free char *remote = NULL;
  gs_free char *ref = NULL; // location of this rev
  char *inbetween = NULL; // necessary addition to URI
  gs_unref_object OstreeRepo *repo = NULL; 
  gs_free char *key = NULL; // key of url in config
  gs_free char *uri = NULL; // full uri location of this ref
  gsize length; // length of checksum in file
  gs_free char *data_from_file = NULL; // the checksum libsoup reads
  GInputStream *gis = NULL;
  GDataInputStream *data_in = NULL;
  SoupSession *session = soup_session_new();
  SoupRequest *req = NULL;
  GPtrArray *deployments = NULL;  // list of all depoyments
  gboolean found_csum_match = FALSE;

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

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
        goto out;

  if (!ostree_parse_refspec (origin_description, &remote, &ref, error))
        goto out;

  if (opt_check_only)
    {
      config = ostree_repo_copy_config (repo);
      key = g_strdup_printf ("remote \"%s\"", remote);
      url = g_key_file_get_string (config, key, "url", error);
      if (url == NULL)
        goto out;

      inbetween = "/refs/heads/";
      uri = g_strdup_printf ("%s%s%s", url, inbetween, ref);

      req = soup_session_request (session, uri, error);
      if (!req)
        goto out;

      gis = soup_request_send (req, cancellable, error);
      if (!gis)
        goto out;
      
      data_in = g_data_input_stream_new (gis);
      if (!data_in)
        goto out;

      data_from_file =  g_data_input_stream_read_line  (data_in, &length, cancellable, error);
      deployments = ostree_sysroot_get_deployments (sysroot);
      for (i=0; i < deployments->len; i++)
        {
          const char *csum = ostree_deployment_get_csum (deployments->pdata[i]);
          if (g_strcmp0 (csum, data_from_file) == 0)
            found_csum_match = TRUE;
        }
      /* if match found, no update available. if no match, remote ahead of local: update available */
      g_print ("Update available: %s\n", found_csum_match ? "false" : "true");
    }

  else
    {
      console = gs_console_get ();
      if (console)
        {
          gs_console_begin_status_line (console, "", NULL, NULL);
          progress = ostree_async_progress_new_and_connect (_rpmostree_pull_progress, console);
        }

      if (opt_allow_downgrade)  
        upgraderpullflags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

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
    #ifdef HAVE_PATCHED_HAWKEY_AND_LIBSOLV
              if (!rpmostree_print_treepkg_diff (sysroot, cancellable, error))
                goto out;
    #endif

              g_print ("Updates prepared for next boot; run \"systemctl reboot\" to start a reboot\n");
            }
        }
    }
  
  ret = TRUE;
 out:
  if (console)
    (void) gs_console_end_status_line (console, NULL, NULL);

  return ret;
}
