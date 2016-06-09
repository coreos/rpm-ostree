/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Anne LoVerso <anne.loverso@students.olin.edu>
 * Copyright (C) 2016 Red Hat, Inc.
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
#include <stdio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>

#include "rpmostree-builtins.h"
#include "rpmostree-dbus-helpers.h"
#include "libsd-locale-util.h"

#include <libglnx.h>

static gboolean opt_pretty;
static gboolean opt_json;

static GOptionEntry option_entries[] = {
  { "pretty", 'p', 0, G_OPTION_ARG_NONE, &opt_pretty, "This option is deprecated and no longer has any effect", NULL },
  { "json", 0, 0, G_OPTION_ARG_NONE, &opt_json, "Output JSON", NULL },
  { NULL }
};

static void
printpad (char c, guint n)
{
  for (guint i = 0; i < n; i++)
    putc (c, stdout);
}

static void
print_kv (const char *key,
          guint       maxkeylen,
          const char *value)
{
  int pad = maxkeylen - strlen (key);
  g_assert (pad >= 0);
  /* +2 for initial leading spaces */
  printpad (' ', pad + 2);
  printf ("%s: %s\n", key, value);
}

/* We will have an optimized path for the case where there are just
 * two deployments, this code will be the generic fallback.
 */
static gboolean
status_generic (RPMOSTreeSysroot *sysroot_proxy,
                RPMOSTreeOS *os_proxy,
                GVariant       *deployments,
                GVariant       *booted_deployment,
                GCancellable   *cancellable,
                GError        **error)
{
  GVariantIter iter;
  const char *booted_id = NULL;
  gboolean first = TRUE;
  const int is_tty = isatty (1);
  const char *red_bold_prefix = is_tty ? "\x1b[31m\x1b[1m" : "";
  const char *red_bold_suffix = is_tty ? "\x1b[22m\x1b[0m" : "";

  if (booted_deployment)
    g_assert (g_variant_lookup (booted_deployment, "id", "&s", &booted_id));

  g_variant_iter_init (&iter, deployments);

  while (TRUE)
    {
      g_autoptr(GVariant) child = g_variant_iter_next_value (&iter);
      g_autoptr(GVariantDict) dict = NULL;
      const gchar *const*origin_packages;
      const gchar *origin_refspec;
      const gchar *id;
      const gchar *os_name;
      const gchar *checksum;
      const gchar *version_string;
      const gchar *unlocked;
      guint64 t = 0;
      int serial;
      gboolean is_booted;
      const guint max_key_len = strlen ("GPGSignature");
      g_autoptr(GVariant) signatures = NULL;
      g_autofree char *timestamp_string = NULL;

      if (child == NULL)
        break;

      dict = g_variant_dict_new (child);      

      /* osname should always be present. */
      g_assert (g_variant_dict_lookup (dict, "osname", "&s", &os_name));
      g_assert (g_variant_dict_lookup (dict, "id", "&s", &id));
      g_assert (g_variant_dict_lookup (dict, "serial", "i", &serial));
      g_assert (g_variant_dict_lookup (dict, "checksum", "s", &checksum));
      g_assert (g_variant_dict_lookup (dict, "timestamp", "t", &t));
      { g_autoptr(GDateTime) timestamp = g_date_time_new_from_unix_utc (t);

        if (timestamp != NULL)
          timestamp_string = g_date_time_format (timestamp, "%Y-%m-%d %T");
        else
          timestamp_string = g_strdup_printf ("(invalid timestamp)");
      }
      
      if (g_variant_dict_lookup (dict, "origin", "s", &origin_refspec))
        {
          if (g_variant_dict_lookup (dict, "packages", "^a&s", &origin_packages))
            {
              /* Canonicalize length 0 strv to NULL */
              if (!*origin_packages)
                origin_packages = NULL;
            }
          else
            {
              origin_packages = NULL;
            }
        }
      else
        origin_refspec = NULL;
      if (!g_variant_dict_lookup (dict, "version", "&s", &version_string))
        version_string = NULL;
      if (!g_variant_dict_lookup (dict, "unlocked", "&s", &unlocked))
        unlocked = NULL;

      signatures = g_variant_dict_lookup_value (dict, "signatures",
                                                G_VARIANT_TYPE ("av"));

      if (first)
        first = FALSE;
      else
        g_print ("\n");

      is_booted = g_strcmp0 (booted_id, id) == 0;

      g_print ("%s ", is_booted ? libsd_special_glyph (BLACK_CIRCLE) : " ");

      if (origin_refspec)
        g_print ("%s", origin_refspec);
      else
        g_print ("%s", checksum);
      g_print ("\n");
      if (version_string)
        {
          g_autofree char *version_time = g_strdup_printf ("%s (%s)",version_string, timestamp_string);
          print_kv ("Version", max_key_len, version_time);
        }
      else
        {
          print_kv ("Timestamp", max_key_len, timestamp_string);
        }
      if (origin_packages)
        {
          const char *base_checksum;
          g_assert (g_variant_dict_lookup (dict, "base-checksum", "&s", &base_checksum));
          print_kv ("BaseCommit", max_key_len, base_checksum);
        }
      print_kv ("Commit", max_key_len, checksum);
      print_kv ("OSName", max_key_len, os_name);

      if (signatures)
        {
          guint n_sigs = g_variant_n_children (signatures);
          g_autofree char *gpgheader = g_strdup_printf ("%u signature%s", n_sigs,
                                                        n_sigs == 1 ? "" : "s");
          const guint gpgpad = max_key_len+4;
          char gpgspaces[gpgpad+1];
          memset (gpgspaces, ' ', gpgpad);
          gpgspaces[gpgpad] = '\0';

          print_kv ("GPGSignature", max_key_len, gpgheader);
          rpmostree_print_signatures (signatures, gpgspaces);
        }
      else
        {
          print_kv ("GPGSignature", max_key_len, "(unsigned)");
        }

      if (origin_packages)
        {
          g_autofree char *packages_joined = g_strjoinv (" ", (char**)origin_packages);
          print_kv ("Packages", max_key_len, packages_joined);
        }
      
      if (unlocked && g_strcmp0 (unlocked, "none") != 0)
        {
          g_print ("%s", red_bold_prefix);
          print_kv ("Unlocked", max_key_len, unlocked);
          g_print ("%s", red_bold_suffix);
        }
    }

  return TRUE;
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

  deployments = rpmostree_sysroot_dup_deployments (sysroot_proxy);
  booted_deployment = rpmostree_os_dup_booted_deployment (os_proxy);

  if (opt_json)
    {
      gsize len;
      g_autofree char *serialized = json_gvariant_serialize_data (deployments, &len);
      g_print ("%s\n", serialized);
    }
  else
    {
      if (!status_generic (sysroot_proxy, os_proxy, deployments,
                           booted_deployment,
                           cancellable, error))
        goto out;
    }

  exit_status = EXIT_SUCCESS;
out:
  /* Does nothing if using the message bus. */
  rpmostree_cleanup_peer ();

  return exit_status;
}
