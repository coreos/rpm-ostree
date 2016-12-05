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
#include <gio/gunixoutputstream.h>
#include <json-glib/json-glib.h>

#include "rpmostree-builtins.h"
#include "rpmostree-dbus-helpers.h"
#include "rpmostree-util.h"
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

static GVariant *
get_active_txn (RPMOSTreeSysroot *sysroot_proxy)
{
  GVariant* txn = rpmostree_sysroot_get_active_transaction (sysroot_proxy);
  const char *a, *b, *c;
  if (txn)
    {
      g_variant_get (txn, "(&s&s&s)", &a, &b, &c);
      if (*a)
        return txn;
    }
  return NULL;
}

/* We will have an optimized path for the case where there are just
 * two deployments, this code will be the generic fallback.
 */
static gboolean
status_generic (RPMOSTreeSysroot *sysroot_proxy,
                RPMOSTreeOS *os_proxy,
                GVariant       *deployments,
                GCancellable   *cancellable,
                GError        **error)
{
  GVariantIter iter;
  gboolean first = TRUE;
  const int is_tty = isatty (1);
  const char *bold_prefix = is_tty ? "\x1b[1m" : "";
  const char *bold_suffix = is_tty ? "\x1b[0m" : "";
  const char *red_prefix = is_tty ? "\x1b[31m" : "";
  const char *red_suffix = is_tty ? "\x1b[22m" : "";
  GVariant* txn = get_active_txn (sysroot_proxy);

  if (txn)
    {
      const char *method, *sender, *path;
      g_variant_get (txn, "(&s&s&s)", &method, &sender, &path);
      g_print ("State: transaction: %s %s %s\n", method, sender, path);
    }
  else
    g_print ("State: idle\n");
  g_print ("Deployments:\n");

  g_variant_iter_init (&iter, deployments);

  while (TRUE)
    {
      g_autoptr(GVariant) child = g_variant_iter_next_value (&iter);
      g_autoptr(GVariantDict) dict = NULL;
      const gchar *const*origin_packages = NULL;
      const gchar *origin_refspec;
      const gchar *id;
      const gchar *os_name;
      const gchar *checksum;
      const gchar *version_string;
      const gchar *unlocked;
      gboolean gpg_enabled;
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

      if (!g_variant_dict_lookup (dict, "booted", "b", &is_booted))
        is_booted = FALSE;

      g_print ("%s ", is_booted ? libsd_special_glyph (BLACK_CIRCLE) : " ");

      if (origin_refspec)
        g_print ("%s", origin_refspec);
      else
        g_print ("%s", checksum);
      g_print ("\n");
      if (version_string)
        {
          g_autofree char *version_time
            = g_strdup_printf ("%s%s%s (%s)", bold_prefix, version_string,
                               bold_suffix, timestamp_string);
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

      if (!g_variant_dict_lookup (dict, "gpg-enabled", "b", &gpg_enabled))
        gpg_enabled = FALSE;

      if (gpg_enabled)
        {
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
        }

      if (origin_packages)
        {
          g_autofree char *packages_joined = NULL;
          g_autoptr(GPtrArray) origin_packages_sorted = g_ptr_array_new ();
          for (char **iter = (char**) origin_packages; iter && *iter; iter++)
            g_ptr_array_add (origin_packages_sorted, *iter);
          g_ptr_array_sort (origin_packages_sorted, rpmostree_ptrarray_sort_compare_strings);
          g_ptr_array_add (origin_packages_sorted, NULL);
          packages_joined = g_strjoinv (" ", (char**)origin_packages_sorted->pdata);
          print_kv ("Packages", max_key_len, packages_joined);
        }
      
      if (unlocked && g_strcmp0 (unlocked, "none") != 0)
        {
          g_print ("%s%s", red_prefix, bold_prefix);
          print_kv ("Unlocked", max_key_len, unlocked);
          g_print ("%s%s", bold_suffix, red_suffix);
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
  g_autoptr(GOptionContext) context = g_option_context_new ("- Get the version of the booted system");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
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

  if (opt_json)
    {
      glnx_unref_object JsonBuilder *builder = json_builder_new ();
      glnx_unref_object JsonGenerator *generator = json_generator_new ();
      JsonNode *deployments_node = json_gvariant_serialize (deployments);
      JsonNode *json_root;
      JsonNode *txn_node;
      glnx_unref_object GOutputStream *stdout_gio = g_unix_output_stream_new (1, FALSE);
      GVariant *txn = get_active_txn (sysroot_proxy);

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "deployments");
      json_builder_add_value (builder, deployments_node);
      json_builder_set_member_name (builder, "transaction");
      if (txn)
        txn_node = json_gvariant_serialize (txn);
      else
        txn_node = json_node_new (JSON_NODE_NULL);
      json_builder_add_value (builder, txn_node);
      json_builder_end_object (builder);
      json_root = json_builder_get_root (builder);
      json_generator_set_root (generator, json_root);
      json_node_free (json_root);

      /* NB: watch out for the misleading API docs */
      if (json_generator_to_stream (generator, stdout_gio, NULL, error) <= 0
          || (error != NULL && *error != NULL))
        goto out;
    }
  else
    {
      if (!status_generic (sysroot_proxy, os_proxy, deployments,
                           cancellable, error))
        goto out;
    }

  exit_status = EXIT_SUCCESS;
out:
  /* Does nothing if using the message bus. */
  rpmostree_cleanup_peer ();

  return exit_status;
}
