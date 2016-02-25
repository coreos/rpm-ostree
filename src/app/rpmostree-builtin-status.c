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

#include <libglnx.h>

#define HEADER_TIMESTAMP "TIMESTAMP (UTC)"
#define HEADER_VERSION   "VERSION"
#define HEADER_ID        "ID"
#define HEADER_OSNAME    "OSNAME"
#define HEADER_REFSPEC   "REFSPEC"

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

static char *
format_layered_packages_plus (char **packages)
{
  guint len = g_strv_length (packages);
  if (len > 0)
    return g_strdup_printf (" (+%u)", len);
  else
    return g_strdup ("");
}

int
rpmostree_builtin_status (int             argc,
                          char          **argv,
                          GCancellable   *cancellable,
                          GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("- Get the version of the booted system");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GVariant) booted_deployment = NULL;
  g_autoptr(GVariant) deployments = NULL;
  g_autoptr(GVariant) booted_signatures = NULL;
  g_autoptr(GPtrArray) deployment_dicts = NULL;
  GVariantIter iter;
  GVariant *child;
  g_autofree gchar *booted_id = NULL;

  const guint CSUM_DISP_LEN = 10; /* number of checksum characters to display */
  guint i, n;

  /* these column maximums are known thus constant */
  guint max_timestamp_len = 19; /* length of timestamp "YYYY-MM-DD HH:MM:SS" */
  guint max_id_len = CSUM_DISP_LEN; /* length of checksum ID */

  /* these column maximums can vary and are thus determined in code */
  /* seed them with the header name length */
  guint max_osname_len = strlen (HEADER_OSNAME);
  guint max_refspec_len = strlen (HEADER_REFSPEC);
  guint max_version_len = strlen (HEADER_VERSION);

  /* minimum space between end of one entry and new column */
  guint buffer = 5;


  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_NONE,
                                       cancellable,
                                       &sysroot_proxy,
                                       error))
    goto out;

  if (!rpmostree_load_os_proxy (sysroot_proxy, NULL,
                                cancellable, &os_proxy, error))
    goto out;

  booted_deployment = rpmostree_os_dup_booted_deployment (os_proxy);
  if (booted_deployment)
    {
      GVariantDict dict;
      g_variant_dict_init (&dict, booted_deployment);
      g_variant_dict_lookup (&dict, "id", "s", &booted_id);
      booted_signatures = g_variant_dict_lookup_value (&dict, "signatures",
                                                       G_VARIANT_TYPE ("av"));
      g_variant_dict_clear (&dict);
    }

  deployment_dicts = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_dict_unref);

  deployments = rpmostree_sysroot_dup_deployments (sysroot_proxy);

  g_variant_iter_init (&iter, deployments);

  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    {
      GVariantDict *dict = g_variant_dict_new (child);

      /* Takes ownership of the dictionary */
      g_ptr_array_add (deployment_dicts, dict);

      /* find lengths for use in column output */
      if (!opt_pretty)
        {
          gchar *origin_refspec = NULL; /* borrowed */
          gchar **origin_packages = NULL; /* borrowed */
          gchar *os_name = NULL; /* borrowed */
          gchar *version_string = NULL; /* borrowed */

          /* osname should always be present. */
          if (g_variant_dict_lookup (dict, "osname", "&s", &os_name))
            max_osname_len = MAX (max_osname_len, strlen (os_name));
          else
            {
              const char *id = NULL;
              g_variant_dict_lookup (dict, "id", "&s", &id);
              g_critical ("Deployment '%s' missing osname", id != NULL ? id : "?");
            }

          if (g_variant_dict_lookup (dict, "version", "&s", &version_string))
            max_version_len = MAX (max_version_len, strlen (version_string));

          if (g_variant_dict_lookup (dict, "origin", "&s", &origin_refspec))
            {
              if (g_variant_dict_lookup (dict, "packages", "^a&s", &origin_packages))
                {
                  g_autofree gchar *origin_packages_plus =
                    format_layered_packages_plus (origin_packages);

                  max_refspec_len = MAX (max_refspec_len, strlen (origin_refspec) + strlen (origin_packages_plus));
                }
              else
                max_refspec_len = MAX (max_refspec_len, strlen (origin_refspec));
            }
        }

      g_variant_unref (child);
    }

  if (!opt_pretty)
    {
      /* print column headers */
      g_print ("  %-*s", max_timestamp_len+buffer, HEADER_TIMESTAMP);
      if (max_version_len)
        g_print ("%-*s", max_version_len+buffer, HEADER_VERSION);
      g_print ("%-*s%-*s%-*s\n",
               max_id_len+buffer, HEADER_ID,
               max_osname_len+buffer, HEADER_OSNAME,
               max_refspec_len, HEADER_REFSPEC);
    }
  /* header for "pretty" row output */
  else
    printchar ("=", 60);

  n = deployment_dicts->len;

  /* print entries for each deployment */
  for (i = 0; i < n; i++)
    {
      GVariantDict *dict;
      g_autoptr(GDateTime) timestamp = NULL;
      g_autofree char *timestamp_string = NULL;
      g_autofree gchar *truncated_csum = NULL;
      g_autoptr(GVariant) signatures = NULL;

      gchar *id = NULL; /* borrowed */
      gchar *origin_refspec = NULL; /* borrowed */
      gchar **origin_packages = NULL; /* borrowed */
      gchar *os_name = NULL; /* borrowed */
      gchar *version_string = NULL; /* borrowed */
      gchar *checksum = NULL; /* borrowed */
      g_autofree gchar *origin_refspec_description = NULL;

      guint64 t = 0;
      gint serial;
      gboolean is_booted = FALSE;

      dict = g_ptr_array_index (deployment_dicts, i);

      g_variant_dict_lookup (dict, "id", "&s", &id);
      g_variant_dict_lookup (dict, "osname", "&s", &os_name);
      g_variant_dict_lookup (dict, "serial", "i", &serial);
      g_variant_dict_lookup (dict, "checksum", "s", &checksum);
      g_variant_dict_lookup (dict, "version", "s", &version_string);
      g_variant_dict_lookup (dict, "timestamp", "t", &t);
      g_variant_dict_lookup (dict, "origin", "s", &origin_refspec);
      g_variant_dict_lookup (dict, "packages", "^a&s", &origin_packages);
      signatures = g_variant_dict_lookup_value (dict, "signatures",
                                                G_VARIANT_TYPE ("av"));

      if (origin_packages)
        {
          g_autofree gchar *origin_packages_plus =
            format_layered_packages_plus (origin_packages);
          origin_refspec_description = g_strconcat (origin_refspec, origin_packages_plus, NULL);
        }
      else
        {
          origin_refspec_description = g_strdup (origin_refspec);
        }

      is_booted = g_strcmp0 (booted_id, id) == 0;

      timestamp = g_date_time_new_from_unix_utc (t);
      if (timestamp != NULL)
        timestamp_string = g_date_time_format (timestamp, "%Y-%m-%d %T");
      else
        timestamp_string = g_strdup_printf ("(invalid)");

      /* truncate checksum */
      truncated_csum = g_strndup (checksum, CSUM_DISP_LEN);

      /* print deployment info column */
      if (!opt_pretty)
        {
          g_print ("%c %-*s",
                   is_booted ? '*' : ' ',
                   max_timestamp_len+buffer, timestamp_string);

          if (max_version_len)
            g_print ("%-*s",
                     max_version_len+buffer, version_string ? version_string : "");
          g_print ("%-*s%-*s%-*s\n",
                   max_id_len+buffer, truncated_csum,
                   max_osname_len+buffer, os_name,
                   max_refspec_len, origin_refspec_description);
        }

      /* print "pretty" row info */
      else
        {
          guint tab = 11;
          char *title = NULL;
          g_autofree char *packages_joined = NULL;

          if (origin_packages && g_strv_length (origin_packages) > 0)
              packages_joined = g_strjoinv (" ", origin_packages);

          if (i==0)
            title = "DEFAULT ON BOOT";
          else if (is_booted || n <= 2)
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

          if (packages_joined)
            g_print ("  %-*s%-*s\n", tab, "packages", tab, packages_joined);

          if (signatures != NULL)
            rpmostree_print_signatures (signatures, "  GPG: ");

          printchar ("=", 60);
        }
    }

  /* Print any signatures for the booted deployment, but only in NON-pretty
   * mode.  We save this for the end to preserve the tabular formatting for
   * deployments. */
  if (!opt_pretty && booted_signatures != NULL)
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

  exit_status = EXIT_SUCCESS;

out:
  /* Does nothing if using the message bus. */
  rpmostree_cleanup_peer ();

  return exit_status;
}
