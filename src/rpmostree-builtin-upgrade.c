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
#include "rpmostree-treepkgdiff.h"
#include "rpmostree-pull-progress.h"
#include "rpmostree-rpm-util.h"

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

gboolean
rpmostree_builtin_upgrade (int             argc,
                           char          **argv,
                           GCancellable   *cancellable,
                           GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("- Perform a system upgrade");
  gs_unref_object GFile *sysroot_path = NULL;
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gs_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  gs_unref_object OstreeAsyncProgress *progress = NULL;
  GSConsole *console = NULL;
  gboolean changed;
  OstreeSysrootUpgraderPullFlags upgraderpullflags = 0;

  gs_free char *origin_description = NULL;
  gs_unref_object OstreeRepo *repo = NULL; 

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  sysroot_path = g_file_new_for_path (opt_sysroot);
  sysroot = ostree_sysroot_new (sysroot_path);
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  upgrader = ostree_sysroot_upgrader_new_for_os (sysroot, opt_osname,
                                                 cancellable, error);
  if (!upgrader)
    goto out;

  origin_description = ostree_sysroot_upgrader_get_origin_description (upgrader);
  if (origin_description)
    g_print ("Updating from: %s\n", origin_description);

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  console = gs_console_get ();
  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (_rpmostree_pull_progress, console);
    }

  if (opt_allow_downgrade)  
    upgraderpullflags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

  if (opt_check_diff)
    {
      if (!ostree_sysroot_upgrader_pull_one_dir (upgrader, "/usr/share/rpm", 0, 0, progress, &changed,
                                     cancellable, error))
        goto out;
    }

  else
    {
      if (!ostree_sysroot_upgrader_pull (upgrader, 0, upgraderpullflags, progress, &changed,
                                         cancellable, error))
        goto out;
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


      else
        {
          gs_unref_object GFile *rpmdbdir = NULL;
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
    }
  
  ret = TRUE;
 out:
  if (console)
    (void) gs_console_end_status_line (console, NULL, NULL);

  return ret;
}
