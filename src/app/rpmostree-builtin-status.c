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
#include "rpmostree-dbus-helpers.h"

#include "libgsystem.h"

static char *opt_sysroot = "/";
static gboolean opt_pretty;

static GOptionEntry option_entries[] = {
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)", "SYSROOT" },
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

static gchar *
format_origin_refspec (RPMOSTreeDeployment *deployment,
                       GDBusConnection *connection,
                       GCancellable *cancellable)
{
  gchar *origin_refspec = NULL;
  gs_free gchar *refspec_path = NULL;
  GError *error = NULL;

  origin_refspec = rpmostree_deployment_dup_origin_refspec (deployment);
  refspec_path = rpmostree_deployment_dup_refspec_objectpath (deployment);

  if (!rpmostree_is_valid_object_path (refspec_path))
      origin_refspec = g_strdup ("none");
  else if (!origin_refspec)
      origin_refspec = g_strdup ("<unknown origin type>");

  // we are printing ignore error
  g_clear_error (&error);
  return origin_refspec;
}


gboolean
rpmostree_builtin_status (int             argc,
                          char          **argv,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("- Get the version of the booted system");
  gs_unref_object GDBusConnection *connection = NULL;
  gs_unref_object RPMOSTreeManager *manager = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_unref_variant GVariant *variant_args = NULL;
  gs_unref_variant GVariant *booted_signatures = NULL;

  gs_free gchar *booted_deployment = NULL;
  gs_strfreev gchar **deployment_paths = NULL;

  const guint CSUM_DISP_LEN = 10; // number of checksum characters to display
  guint i, j, n;
  guint max_timestamp_len = 19; // length of timestamp "YYYY-MM-DD HH:MM:SS"
  guint max_id_len = CSUM_DISP_LEN; // length of checksum ID
  guint max_osname_len = 0; // maximum length of osname - determined in conde
  guint max_refspec_len = 0; // maximum length of refspec - determined in code
  guint max_version_len = 0; // maximum length of version - determined in code
  guint buffer = 5; // minimum space between end of one entry and new column


  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, error))
    goto out;

  if (!opt_sysroot)
    opt_sysroot = "/";

  connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, cancellable, error);
  if (!connection)
    goto out;

  // Get manager
  manager = rpmostree_manager_proxy_new_sync (connection,
                                              G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                              BUS_NAME,
                                              "/org/projectatomic/rpmostree1/Manager",
                                              cancellable,
                                              error);
  if (!manager)
    goto out;

  // populate deployment information
  variant_args = g_variant_ref_sink (g_variant_new ("a{sv}", NULL));
  booted_deployment = rpmostree_manager_dup_booted_deployment (manager);
  if (!rpmostree_manager_call_get_deployments_sync (manager,
                                                    variant_args,
                                                    &deployment_paths,
                                                    cancellable,
                                                    error))
    goto out;

  deployments = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  n = g_strv_length (deployment_paths);
  for (j = 0; j < n; j++)
    {
      RPMOSTreeDeployment *deployment = rpmostree_deployment_proxy_new_sync (connection,
                                                  G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                  BUS_NAME,
                                                  deployment_paths[j],
                                                  cancellable,
                                                  error);
      if (deployment)
        {
          g_ptr_array_add (deployments, deployment);

          /* find lengths for use in column output */
          if(!opt_pretty)
            {
              gs_free gchar *origin_refspec = NULL;
              gs_free gchar *os_name = NULL;
              gs_free gchar *version_string = NULL;
              gs_free gchar *checksum = NULL;

              os_name = rpmostree_deployment_dup_osname (deployment);
              version_string = rpmostree_deployment_dup_commit (deployment);
              origin_refspec = format_origin_refspec (deployment, connection, cancellable);
              max_osname_len = MAX (max_osname_len, strlen (os_name));
              max_refspec_len = MAX (max_refspec_len, strlen (origin_refspec));
              if (version_string)
                max_version_len = MAX (max_version_len, strlen (version_string));
            }
        }
    }

  if(!opt_pretty)
    {
      /* print column headers */
      g_print ("  %-*s", max_timestamp_len+buffer,"TIMESTAMP (UTC)");
      if (max_version_len)
        g_print ("%-*s", max_version_len+buffer,"VERSION");
      g_print ("%-*s%-*s%-*s\n",
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
      gs_free gchar *origin_refspec = NULL;
      gs_free gchar *os_name = NULL;
      gs_free gchar *version_string = NULL;
      gs_free gchar *checksum = NULL;
      GDateTime *timestamp = NULL;
      gs_free char *timestamp_string = NULL;
      gs_free gchar *truncated_csum = NULL;
      gs_free gchar *dbus_path = NULL;
      gs_unref_variant GVariant *signatures = NULL;
      RPMOSTreeDeployment *deployment = deployments->pdata[i];
      gint64 t;
      gboolean is_booted = FALSE;

      g_object_get (deployment, "g-object-path", &dbus_path, NULL);
      is_booted = g_strcmp0 (booted_deployment, dbus_path) == 0;

      signatures = rpmostree_deployment_dup_signatures (deployment);
      os_name = rpmostree_deployment_dup_osname (deployment);
      checksum = rpmostree_deployment_dup_checksum (deployment);
      version_string = rpmostree_deployment_dup_commit (deployment);
      origin_refspec = format_origin_refspec (deployment, connection, cancellable);

      t = rpmostree_deployment_get_timestamp (deployment);
      timestamp = g_date_time_new_from_unix_utc (t);
      g_assert (timestamp);
      timestamp_string = g_date_time_format (timestamp, "%Y-%m-%d %T");
      g_date_time_unref (timestamp);

      /* truncate checksum */
      truncated_csum = g_strndup (checksum, CSUM_DISP_LEN);

      /* print deployment info column */
      if (!opt_pretty)
        {
          /* Stash this for printing signatures later. */
          if (is_booted)
            booted_signatures = g_variant_ref (signatures);

          g_print ("%c %-*s",
                   is_booted ? '*' : ' ',
                   max_timestamp_len+buffer, timestamp_string);

          if (max_version_len)
            g_print ("%-*s",
                     max_version_len+buffer, version_string ? version_string : "");
          g_print ("%-*s%-*s%-*s\n",
                   max_id_len+buffer, truncated_csum,
                   max_osname_len+buffer, os_name,
                   max_refspec_len+buffer, origin_refspec);
        }

      /* print "pretty" row info */
      else
        {
          guint n_sigs;
          guint tab = 11;
          gint serial = rpmostree_deployment_get_serial (deployment);
          char *title = NULL;
          if (i==0)
            title = "DEFAULT ON BOOT";
          else if (is_booted ||
                   deployments->len <= 2)
            title = "NON-DEFAULT ROLLBACK TARGET";
          else
            title = "NON-DEFAULT DEPLOYMENT";
          g_print ("  %c %s\n",
                  is_booted ? '*' : ' ',
                  title);

          printchar ("-", 40);
          if (version_string)
            g_print ("  %-*s%-*s\n", tab, "version", tab, version_string);

          g_print ("  %-*s%-*s\n  %-*s%-*s.%d\n  %-*s%-*s\n  %-*s%-*s\n",
                  tab, "timestamp", tab, timestamp_string,
                  tab, "id", tab, checksum, serial,
                  tab, "osname", tab, os_name,
                  tab, "refspec", tab, origin_refspec);

          n_sigs = g_variant_n_children (signatures);
          if (n_sigs > 0)
              rpmostree_print_signatures (signatures, "  GPG: ");

          printchar ("=", 60);
        }
    }

  /* Print any signatures for the booted deployment, but only in NON-pretty
   * mode.  We save this for the end to preserve the tabular formatting for
   * deployments. */
  if (booted_signatures != NULL)
    {
      guint n_sigs = g_variant_n_children (booted_signatures);
      if (n_sigs > 0)
        {
          /* XXX If we ever add internationalization, use ngettext() here. */
          g_print ("\nGPG: Found %u signature%s on the booted deployment (*):\n",
                   n_sigs, n_sigs == 1 ? "" : "s");
          rpmostree_print_signatures (booted_signatures, "  ");
        }
    }

  ret = TRUE;
  out:
  	return ret;
}
