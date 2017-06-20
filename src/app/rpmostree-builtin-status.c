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
static gboolean opt_verbose;
static gboolean opt_json;

static GOptionEntry option_entries[] = {
  { "pretty", 'p', 0, G_OPTION_ARG_NONE, &opt_pretty, "This option is deprecated and no longer has any effect", NULL },
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print additional fields (e.g. StateRoot)", NULL },
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

static void
print_packages (const char *k, guint max_key_len,
                const char *const* pkgs,
                const char *const* omit_pkgs)
{
  g_autofree char *packages_joined = NULL;
  g_autoptr(GPtrArray) packages_sorted =
    g_ptr_array_new_with_free_func (g_free);

  static gsize regex_initialized;
  static GRegex *safe_chars_regex;

  if (g_once_init_enter (&regex_initialized))
    {
      safe_chars_regex = g_regex_new ("^[[:alnum:]-._]+$", 0, 0, NULL);
      g_assert (safe_chars_regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  for (char **iter = (char**) pkgs; iter && *iter; iter++)
    {
      if (omit_pkgs != NULL && g_strv_contains (omit_pkgs, *iter))
        continue;

      /* don't quote if it just has common pkgname/shell-safe chars */
      if (g_regex_match (safe_chars_regex, *iter, 0, 0))
        g_ptr_array_add (packages_sorted, g_strdup (*iter));
      else
        g_ptr_array_add (packages_sorted, g_shell_quote (*iter));
    }

  if (packages_sorted->len > 0)
    {
      g_ptr_array_sort (packages_sorted, rpmostree_ptrarray_sort_compare_strings);
      g_ptr_array_add (packages_sorted, NULL);
      packages_joined = g_strjoinv (" ", (char**)packages_sorted->pdata);
      print_kv (k, max_key_len, packages_joined);
    }
}

static const gchar**
lookup_array_and_canonicalize (GVariantDict *dict,
                               const char   *key)
{
  g_autofree const gchar **ret = NULL;

  if (g_variant_dict_lookup (dict, key, "^a&s", &ret))
    {
      /* Canonicalize length 0 strv to NULL */
      if (!*ret)
        g_clear_pointer (&ret, g_free);
    }

  return g_steal_pointer (&ret);
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
  const char *txn_path = rpmostree_sysroot_get_active_transaction_path (sysroot_proxy);

  /* First, gather global state */
  gboolean have_any_live_overlay = FALSE;
  g_variant_iter_init (&iter, deployments);
  while (TRUE)
    {
      g_autoptr(GVariant) child = g_variant_iter_next_value (&iter);

      if (!child)
        break;

      g_autoptr(GVariantDict) dict = g_variant_dict_new (child);

      const gchar *live_inprogress;
      if (!g_variant_dict_lookup (dict, "live-inprogress", "&s", &live_inprogress))
        live_inprogress = NULL;
      const gchar *live_replaced;
      if (!g_variant_dict_lookup (dict, "live-replaced", "&s", &live_replaced))
        live_replaced = NULL;
      const gboolean have_live_changes = live_inprogress || live_replaced;

      have_any_live_overlay = have_any_live_overlay || have_live_changes;
    }

  if (txn && txn_path)
    {
      const char *method, *sender, *path;
      g_variant_get (txn, "(&s&s&s)", &method, &sender, &path);

      /* Things currently could race here if the transaction completes after we get the
       * path.  For now, just ignore errors.  TODO: A more correct fix would involve a loop
       * on watching the path property, trying a connection, and re-reading the value
       * and only erroring out if the property hasn't changed.
       */
      /* gdbus-codegen started generating autocleanups from 2.50 */
      glnx_unref_object RPMOSTreeTransactionProxy *txn_proxy =
        (RPMOSTreeTransactionProxy*)rpmostree_transaction_connect (txn_path, NULL, NULL);
      if (txn_proxy)
        {
          const char *title = rpmostree_transaction_get_title ((RPMOSTreeTransaction*)txn_proxy);
          g_print ("State: transaction: %s\n", title);
        }
      /* Print the address if verbose, *or* if we somehow failed to get
       * the txn, so we aren't masking errors.
       */
      if (opt_verbose || (path && !txn_proxy))
        g_print ("TransactionAddress: %s %s %s\n", method, sender, path);
    }
  else
    g_print ("State: idle\n");
  g_print ("Deployments:\n");

  g_variant_iter_init (&iter, deployments);

  while (TRUE)
    {
      g_autoptr(GVariant) child = g_variant_iter_next_value (&iter);
      g_autoptr(GVariantDict) dict = NULL;
      gboolean is_locally_assembled = FALSE;
      g_autofree const gchar **origin_packages = NULL;
      g_autofree const gchar **origin_requested_packages = NULL;
      g_autofree const gchar **origin_requested_local_packages = NULL;
      g_autofree const gchar **origin_base_removals = NULL;
      g_autofree const gchar **origin_requested_base_removals = NULL;
      const gchar *origin_refspec;
      const gchar *id;
      const gchar *os_name;
      const gchar *checksum;
      const gchar *version_string;
      const gchar *unlocked;
      const gchar *live_inprogress;
      const gchar *live_replaced;
      gboolean gpg_enabled;
      gboolean regenerate_initramfs;
      guint64 t = 0;
      int serial;
      gboolean is_booted;
      const gboolean was_first = first;
      /* Add the long keys here */
      const guint max_key_len = MAX (strlen ("RemovedBasePackages"),
                                     strlen ("InterruptedLiveCommit"));
      g_autoptr(GVariant) signatures = NULL;
      g_autofree char *timestamp_string = NULL;

      if (child == NULL)
        break;

      dict = g_variant_dict_new (child);

      /* osname should always be present. */
      g_assert (g_variant_dict_lookup (dict, "osname", "&s", &os_name));
      g_assert (g_variant_dict_lookup (dict, "id", "&s", &id));
      g_assert (g_variant_dict_lookup (dict, "serial", "i", &serial));
      g_assert (g_variant_dict_lookup (dict, "checksum", "&s", &checksum));

      if (g_variant_dict_lookup (dict, "origin", "&s", &origin_refspec))
        {
          origin_packages =
            lookup_array_and_canonicalize (dict, "packages");
          origin_requested_packages =
            lookup_array_and_canonicalize (dict, "requested-packages");
          origin_requested_local_packages =
            lookup_array_and_canonicalize (dict, "requested-local-packages");
          origin_base_removals =
            lookup_array_and_canonicalize (dict, "base-removals");
          origin_requested_base_removals =
            lookup_array_and_canonicalize (dict, "requested-base-removals");
        }
      else
        origin_refspec = NULL;
      if (!g_variant_dict_lookup (dict, "version", "&s", &version_string))
        version_string = NULL;
      if (!g_variant_dict_lookup (dict, "unlocked", "&s", &unlocked))
        unlocked = NULL;

      if (!g_variant_dict_lookup (dict, "regenerate-initramfs", "b", &regenerate_initramfs))
        regenerate_initramfs = FALSE;

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

      const char *base_checksum = NULL;
      g_variant_dict_lookup (dict, "base-checksum", "&s", &base_checksum);
      if (base_checksum != NULL)
        is_locally_assembled = TRUE;

      if (is_locally_assembled)
        g_assert (g_variant_dict_lookup (dict, "base-timestamp", "t", &t));
      else
        g_assert (g_variant_dict_lookup (dict, "timestamp", "t", &t));
      { g_autoptr(GDateTime) timestamp = g_date_time_new_from_unix_utc (t);

        if (timestamp != NULL)
          timestamp_string = g_date_time_format (timestamp, "%Y-%m-%d %T");
        else
          timestamp_string = g_strdup_printf ("(invalid timestamp)");
      }

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

      if (!g_variant_dict_lookup (dict, "live-inprogress", "&s", &live_inprogress))
        live_inprogress = NULL;
      if (!g_variant_dict_lookup (dict, "live-replaced", "&s", &live_replaced))
        live_replaced = NULL;
      const gboolean have_live_changes = live_inprogress || live_replaced;

      if (is_locally_assembled)
        {
          if (have_live_changes)
            print_kv ("BootedBaseCommit", max_key_len, base_checksum);
          else
            print_kv ("BaseCommit", max_key_len, base_checksum);
          if (opt_verbose || have_any_live_overlay)
            print_kv ("Commit", max_key_len, checksum);
        }
      else
        {
          if (have_live_changes)
            print_kv ("BootedCommit", max_key_len, checksum);
          if (!have_live_changes || opt_verbose)
            print_kv ("Commit", max_key_len, checksum);
        }

      if (live_inprogress)
        {
          if (is_booted)
            g_print ("%s%s", red_prefix, bold_prefix);
          print_kv ("InterruptedLiveCommit", max_key_len, live_inprogress);
          if (is_booted)
            g_print ("%s%s", bold_suffix, red_suffix);
        }
      if (live_replaced)
        {
          if (is_booted)
            g_print ("%s%s", red_prefix, bold_prefix);
          print_kv ("LiveCommit", max_key_len, live_replaced);
          if (is_booted)
            g_print ("%s%s", bold_suffix, red_suffix);
        }

      /* Show any difference between the baseref vs head, but only for the
         booted commit, and only if there isn't a pending deployment. Otherwise
         it's either unnecessary or too noisy.
      */
      if (is_booted && was_first)
        {
          const gchar *pending_checksum = NULL;
          const gchar *pending_version = NULL;

          if (g_variant_dict_lookup (dict, "pending-base-checksum", "&s", &pending_checksum))
            {
              print_kv (is_locally_assembled ? "PendingBaseCommit" : "PendingCommit",
                        max_key_len, pending_checksum);
              g_assert (g_variant_dict_lookup (dict, "pending-base-timestamp", "t", &t));
              g_variant_dict_lookup (dict, "pending-base-version", "&s", &pending_version);

              if (pending_version)
                {
                  g_autoptr(GDateTime) timestamp = g_date_time_new_from_unix_utc (t);
                  g_autofree char *version_time = NULL;

                  if (timestamp != NULL)
                    timestamp_string = g_date_time_format (timestamp, "%Y-%m-%d %T");
                  else
                    timestamp_string = g_strdup_printf ("(invalid timestamp)");

                  version_time = g_strdup_printf ("%s (%s)", pending_version, timestamp_string);
                  print_kv (is_locally_assembled ? "PendingBaseVersion" : "PendingVersion",
                            max_key_len, version_time);
                }
            }
        }

      /* This used to be OSName; see https://github.com/ostreedev/ostree/pull/794 */
      if (opt_verbose)
        print_kv ("StateRoot", max_key_len, os_name);

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

      /* print base overrides before overlays */
      if (origin_base_removals)
        print_packages ("RemovedBasePackages", max_key_len, origin_base_removals, NULL);

      /* only print inactive base removal requests in verbose mode */
      if (origin_requested_base_removals && opt_verbose)
        print_packages ("InactiveBaseRemovals", max_key_len,
                        origin_requested_base_removals, origin_base_removals);

      /* only print inactive layering requests in verbose mode */
      if (origin_requested_packages && opt_verbose)
        /* requested-packages - packages = inactive (i.e. dormant requests) */
        print_packages ("InactiveRequests", max_key_len,
                        origin_requested_packages, origin_packages);

      if (origin_packages)
        print_packages ("LayeredPackages", max_key_len,
                        origin_packages, NULL);

      if (origin_requested_local_packages)
        print_packages ("LocalPackages", max_key_len,
                        origin_requested_local_packages, NULL);

      if (regenerate_initramfs)
        {
          g_autoptr(GString) buf = g_string_new ("");
          g_autofree char **initramfs_args = NULL;

          g_variant_dict_lookup (dict, "initramfs-args", "^a&s", &initramfs_args);

          for (char **iter = initramfs_args; iter && *iter; iter++)
            {
              g_string_append (buf, *iter);
              g_string_append_c (buf, ' ');
            }
          if (buf->len == 0)
            g_string_append (buf, "regenerate");
          print_kv ("Initramfs", max_key_len, buf->str);
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
                          RpmOstreeCommandInvocation *invocation,
                          GCancellable   *cancellable,
                          GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("- Get the version of the booted system");
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autoptr(GVariant) deployments = NULL;
  _cleanup_peer_ GPid peer_pid = 0;

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL,
                                       &sysroot_proxy,
                                       &peer_pid,
                                       error))
    return EXIT_FAILURE;

  if (!rpmostree_load_os_proxy (sysroot_proxy, NULL,
                                cancellable, &os_proxy, error))
    return EXIT_FAILURE;

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
        return EXIT_FAILURE;
    }
  else
    {
      if (!status_generic (sysroot_proxy, os_proxy, deployments,
                           cancellable, error))
        return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
