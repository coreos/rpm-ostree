/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Anne LoVerso <anne.loverso@students.olin.edu>
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
#include "rpmostree-treepkgdiff.h"

#include "libgsystem.h"

static gboolean opt_pretty;

static GOptionEntry option_entries[] = {
  { "pretty", 'p', 0, G_OPTION_ARG_NONE, &opt_pretty, "Display status in formatted rows", NULL },
  { NULL }
};

static void 
printchar (char *s, int n)
{
  int i;
  for (i=0; i < n; i++)
    g_print ("%s",s);
  g_print ("\n");
}

gboolean
rpmostree_builtin_status (int             argc,
                          char          **argv,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;  // list of all depoyments
  OstreeDeployment *booted_deployment = NULL;   // current booted deployment
  GOptionContext *context = g_option_context_new ("- Get the version of the booted system");
  const guint CSUM_DISP_LEN = 10; // number of checksum characters to display
  guint i, j;
  guint max_timestamp_len = 19; // length of timestamp "YYYY-MM-DD HH:MM:SS"
  guint max_id_len = CSUM_DISP_LEN; // length of checksum ID
  guint max_osname_len = 0; // maximum length of osname - determined in conde
  guint max_refspec_len = 0; // maximum length of refspec - determined in code
  guint buffer = 5; // minimum space between end of one entry and new column

  g_option_context_add_main_entries (context, option_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  sysroot = ostree_sysroot_new_default ();
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  if (booted_deployment == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system");
        goto out;
    }

  deployments = ostree_sysroot_get_deployments (sysroot);
  g_assert (booted_deployment != NULL);

  /* find lengths for use in column output */
  if(!opt_pretty)
    {
      /* find max lengths of osname and refspec */
      for (j = 0; j < deployments->len; j++) 
        {
          GKeyFile *origin;
          gs_free char *origin_refspec = NULL;
          OstreeDeployment *deployment = deployments->pdata[j];

          max_osname_len = MAX (max_osname_len, strlen (ostree_deployment_get_osname (deployment)));

          origin = ostree_deployment_get_origin (deployment);
          if (!origin)
            origin_refspec = "none";
          else
            {
              origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
              if (!origin_refspec)
                origin_refspec = "<unknown origin type>";
            }
          max_refspec_len = MAX (max_refspec_len, strlen (origin_refspec));
        }
      /* print column headers */
      g_print ("  %-*s%-*s%-*s%-*s\n", 
              max_timestamp_len+buffer,"TIMESTAMP (UTC)",
              max_id_len+buffer, "ID",
              max_osname_len+buffer, "OSNAME",
              max_refspec_len+buffer, "REFSPEC");
    }
  /* header for "pretty" row output */
  else
    printchar ("=", 60);

  /* print entries for each deployment */
  for (i=0; i<deployments->len; i++)
    {
      gs_unref_variant GVariant *version = NULL;
      gs_unref_object OstreeRepo *repo = NULL;
      const char *csum = ostree_deployment_get_csum (deployments->pdata[i]);
      OstreeDeployment *deployment = deployments->pdata[i];
      GKeyFile *origin;
      gs_free char *origin_refspec = NULL;
      GDateTime *timestamp = NULL;
      gs_free char *timestamp_string = NULL;
      char *truncated_csum = NULL;

      /* get version for timestamp */
      if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
        goto out;
      if (!ostree_repo_load_variant (repo,
                                   OSTREE_OBJECT_TYPE_COMMIT,
                                   csum,
                                   &version,
                                   error))
        goto out;

      /* format timestamp*/
      timestamp = g_date_time_new_from_unix_utc (ostree_commit_get_timestamp (version));
      g_assert (timestamp);
      timestamp_string = g_date_time_format (timestamp, "%Y-%m-%d %T");
      g_date_time_unref (timestamp);

      /* get origin refspec */
      origin = ostree_deployment_get_origin (deployment);
      if (!origin)
        origin_refspec = "none";
      else
        {
          origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
          if (!origin_refspec)
            origin_refspec = "<unknown origin type>";
        }

      /* truncate checksum */
      truncated_csum = g_strndup (csum, CSUM_DISP_LEN);

      /* print deployment info column */
      if (!opt_pretty)
        {
          g_print ("%c %-*s%-*s%-*s%-*s\n", 
                  deployment == booted_deployment ? '*' : ' ',
                  max_timestamp_len+buffer, timestamp_string,
                  max_id_len+buffer, truncated_csum,
                  max_osname_len+buffer, ostree_deployment_get_osname (deployment),
                  max_refspec_len+buffer, origin_refspec);
        }

      /* print "pretty" row info */
      else
        {
          guint tab = 11;
          char *title = NULL;
          if (i==0)
            title = "DEFAULT ON BOOT";
          else if (deployment == booted_deployment ||
                  deployments->len <= 2)
            title = "NON-DEFAULT ROLLBACK TARGET";
          else
            title = "NON-DEFAULT DEPLOYMENT";
          g_print ("  %c %s\n",
                  deployment == booted_deployment ? '*' : ' ',
                  title);

          printchar ("-", 40);
          g_print ("  %-*s%-*s\n  %-*s%-*s.%d\n  %-*s%-*s\n  %-*s%-*s\n",
                  tab, "timestamp", tab, timestamp_string,
                  tab, "id", tab, csum, ostree_deployment_get_deployserial (deployment),
                  tab, "osname", tab, ostree_deployment_get_osname (deployment),
                  tab, "refspec", tab, origin_refspec);
          printchar ("=", 60);
        }
    }

  ret = TRUE;
  out:
  	return ret;
}
