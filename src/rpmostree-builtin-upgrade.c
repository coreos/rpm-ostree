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

/**
 * SECTION:libostree-sysroot-upgrader
 * @title: Simple upgrade class
 * @short_description: Upgrade OSTree systems
 *
 * The #OstreeSysrootUpgrader class allows performing simple upgrade
 * operations.
 */
 /*
 This struct is copy-pasted from ostree-sysroot-upgrader.c
 because without it, opt_check_diff would not work
 */
struct OstreeSysrootUpgrader {
  GObject parent;

  OstreeSysroot *sysroot;
  char *osname;

  OstreeDeployment *merge_deployment;
  GKeyFile *origin;
  char *origin_remote;
  char *origin_ref;

  char *new_revision;
}; 

static gboolean opt_reboot;
static gboolean opt_allow_downgrade;
static gboolean opt_check_only;
static gboolean opt_check_diff;

static GOptionEntry option_entries[] = {
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after an upgrade is prepared", NULL },
  { "allow-downgrade", 0, 0, G_OPTION_ARG_NONE, &opt_allow_downgrade, "Permit deployment of chronologically older trees", NULL },
  { "check-only", 'c', 0, G_OPTION_ARG_NONE, &opt_check_only, "Check for upgrades and print true/false only", NULL },
  { "check-diff", 0, 0, G_OPTION_ARG_NONE, &opt_check_diff, "Check for upgrades and print package diff only", NULL },
  { NULL }
};

/**
 * given a repo and an origin_description,
 * this uses libsoup to access the remote
 * repo and read the checksum of the 
 * commit version
 */
static gboolean
get_checksum_from_repo (OstreeRepo    *repo,
                        char          *origin_description,
                        char         **out_checksum,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gboolean ret = FALSE;

  gs_free char *checksum_from_file = NULL;
  GKeyFile *config = NULL; // config file, holds URL
  GDataInputStream *data_in = NULL;
  GInputStream *gis = NULL;
  char *inbetween = NULL; // necessary addition to URI
  gs_free char *key = NULL; // key of url in config
  gsize length; // length of checksum in file
  gs_free char *ref = NULL; // location of this rev
  gs_free char *remote = NULL;
  SoupRequest *req = NULL;
  SoupSession *session = soup_session_new();
  gs_free char *uri = NULL; // full uri location of this ref
  gs_free char *url = NULL; // url from config file

  if (!ostree_parse_refspec (origin_description, &remote, &ref, error))
        goto out;

  config = ostree_repo_copy_config (repo);
  key = g_strdup_printf ("remote \"%s\"", remote);
  url = g_key_file_get_string (config, key, "url", error);
  if (url == NULL)
    goto out;

  inbetween = "/refs/heads/";
  uri = g_strconcat (url, inbetween, ref, NULL);

  req = soup_session_request (session, uri, error);
  if (!req)
    goto out;

  gis = soup_request_send (req, cancellable, error);
  if (!gis)
    goto out;
  
  data_in = g_data_input_stream_new (gis);
  if (!data_in)
    goto out;

  checksum_from_file =  g_data_input_stream_read_line  (data_in, &length, cancellable, error);
  if (!checksum_from_file)
    goto out;

  ret = TRUE;
  gs_transfer_out_value (out_checksum, &checksum_from_file);
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
  GSConsole *console = NULL;
  gboolean changed;
  OstreeSysrootUpgraderPullFlags upgraderpullflags = 0;

  gboolean already_deployed = FALSE;
  gboolean already_downloaded = FALSE;
  GPtrArray *deployments = NULL;  // list of all depoyments
  gboolean found_csum_match = FALSE;
  guint i, j;
  gs_free char *origin_description = NULL;
  gs_unref_object OstreeRepo *repo = NULL; 

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

  deployments = ostree_sysroot_get_deployments (sysroot);

  if (opt_check_diff)
    opt_check_only = TRUE;

  if (opt_check_only)
    {
      char *data_from_file = NULL; // the checksum libsoup reads

      if (!get_checksum_from_repo (repo, origin_description, &data_from_file, cancellable, error))
        goto out;

      for (i=0; i < deployments->len; i++)
        {
          const char *csum = ostree_deployment_get_csum (deployments->pdata[i]);
          if (g_strcmp0 (csum, data_from_file) == 0)
            found_csum_match = TRUE;
        }
      /* if match found, no update available. if no match, remote ahead of local: update available */
      g_print ("Update available: %s\n", found_csum_match ? "false" : "true");
    }

  if ((opt_check_diff && !found_csum_match) || !opt_check_only)
    {
      char *checksum_from_repo;
      gs_unref_variant GVariant *version = NULL;

      console = gs_console_get ();
      if (console)
        {
          gs_console_begin_status_line (console, "", NULL, NULL);
          progress = ostree_async_progress_new_and_connect (_rpmostree_pull_progress, console);
        }

      if (opt_allow_downgrade)  
        upgraderpullflags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

      if (!get_checksum_from_repo (repo, origin_description, &checksum_from_repo, cancellable, error))
        goto out;

      if (!ostree_repo_load_variant_if_exists (repo,
                                               OSTREE_OBJECT_TYPE_COMMIT,
                                               checksum_from_repo,
                                               &version,
                                               error))
            goto out;
      
      /* If ostree_repo_load_variant_if_exists returned version as
      anything other than null, then the commit exists and we've already
      downloaded this version */
      if (version)
        {
          already_downloaded = TRUE;
          for (j=0; j < deployments->len; j++)
              {
                /* this checks if the repo checksum matches any of the DEPLOYMENTS
                (instead of just ref checksums) */
                const char *csum = ostree_deployment_get_csum (deployments->pdata[j]);
                if (g_strcmp0 (csum, checksum_from_repo) == 0)
                  already_deployed = TRUE;
              }
        }
      
      /* only pull if we haven't already downloaded
         OR if we already deployed, which means either:
         a) current deployment is most recent version, "changed" will be false, or
         b)  we rolled back and current dpeloyment is not most recent - following
         normal functionality of upgrade it will pull and deploy the upgrade with
         an incremented deploy serial 
         NOTE - in situation b running --check-only will indicate "update available: false" */
      if (!already_downloaded || already_deployed)
        {
          if (!ostree_sysroot_upgrader_pull (upgrader, 0, 0, progress, &changed,
                                           cancellable, error))
            goto out;
        }
      /* update the upgrader with the checksum of the already-downloaded version
      this would have been down in upgrader_pull but we skipped that 
      this way, the upgrader knows what to deploy in deploy_tree */
      else
        {
          char *checksum_of_download = NULL;
          if (!get_checksum_from_repo (repo, origin_description, &checksum_of_download, cancellable, error))
            goto out;
          upgrader->new_revision = checksum_of_download;
          changed = TRUE;
        }

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
          if (!opt_check_diff)
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
          /* check-diff option that does NOT write deployment
          instead just prints package diff */
          else
            {
              OstreeDeployment *new_deployment = NULL;
              OstreeDeployment *current_deployment = ostree_sysroot_get_booted_deployment (sysroot);
              gs_unref_object GFile *current_root = NULL;
              gs_unref_object GFile *new_root = NULL;

              if (!ostree_sysroot_deploy_tree (upgrader->sysroot, upgrader->osname,
                                               upgrader->new_revision,
                                               upgrader->origin,
                                               upgrader->merge_deployment,
                                               NULL,
                                               &new_deployment,
                                               cancellable, error))
                goto out;

              current_root = ostree_sysroot_get_deployment_directory (sysroot, current_deployment);
              new_root = ostree_sysroot_get_deployment_directory (sysroot, new_deployment);

              if (!print_rpmdb_diff (current_root, new_root, cancellable, error))
                goto out;
            }
        }
    }
  
  ret = TRUE;
 out:
  if (console)
    (void) gs_console_end_status_line (console, NULL, NULL);

  return ret;
}