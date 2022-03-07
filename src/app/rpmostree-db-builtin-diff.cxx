/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 James Antil <james@fedoraproject.org>
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

#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>

#include "rpmostree-db-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree.h"

static char *opt_format;
static gboolean opt_changelogs;
static char *opt_sysroot;
static gboolean opt_base;
static gboolean opt_advisories;

static GOptionEntry option_entries[] = {
  { "format", 'F', 0, G_OPTION_ARG_STRING, &opt_format,
    "Output format: \"diff\" or \"json\" or (default) \"block\"", "FORMAT" },
  { "changelogs", 'c', 0, G_OPTION_ARG_NONE, &opt_changelogs, "Also output RPM changelogs", NULL },
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Use system root SYSROOT (default: /)",
    "SYSROOT" },
  { "base", 0, 0, G_OPTION_ARG_NONE, &opt_base,
    "Diff against deployments' base, not layered commits", NULL },
  { "advisories", 'a', 0, G_OPTION_ARG_NONE, &opt_advisories, "Also output new advisories", NULL },
  { NULL }
};

static gboolean
print_diff (OstreeRepo *repo, const char *from_desc, const char *from_checksum, const char *to_desc,
            const char *to_checksum, GCancellable *cancellable, GError **error)
{
  const gboolean is_diff_format = g_str_equal (opt_format, "diff");

  if (!g_str_equal (from_desc, from_checksum))
    printf ("ostree diff commit from: %s (%s)\n", from_desc, from_checksum);
  else
    printf ("ostree diff commit from: %s\n", from_desc);

  if (!g_str_equal (to_desc, to_checksum))
    printf ("ostree diff commit to:   %s (%s)\n", to_desc, to_checksum);
  else
    printf ("ostree diff commit to:   %s\n", to_desc);

  g_autoptr (GPtrArray) removed = NULL;
  g_autoptr (GPtrArray) added = NULL;
  g_autoptr (GPtrArray) modified_from = NULL;
  g_autoptr (GPtrArray) modified_to = NULL;

  /* we still use the old API for changelogs; should enhance libdnf for this */
  if (!is_diff_format && opt_changelogs)
    {
      g_autoptr (RpmRevisionData) rpmrev1
          = rpmrev_new (repo, from_checksum, NULL, cancellable, error);
      if (!rpmrev1)
        return FALSE;
      g_autoptr (RpmRevisionData) rpmrev2
          = rpmrev_new (repo, to_checksum, NULL, cancellable, error);
      if (!rpmrev2)
        return FALSE;

      rpmhdrs_diff_prnt_block (
          TRUE, rpmhdrs_diff (rpmrev_get_headers (rpmrev1), rpmrev_get_headers (rpmrev2)));
    }
  else
    {
      if (!rpm_ostree_db_diff (repo, from_checksum, to_checksum, &removed, &added, &modified_from,
                               &modified_to, cancellable, error))
        return FALSE;

      if (is_diff_format)
        rpmostree_diff_print (removed, added, modified_from, modified_to);
      else
        rpmostree_diff_print_formatted (RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE, NULL, 0,
                                        removed, added, modified_from, modified_to);
    }

  if (opt_advisories)
    {
      auto diff
          = CXX_TRY_VAL (calculate_advisories_diff (*repo, from_checksum, to_checksum), error);
      g_print ("\n");
      rpmostree_print_advisories (diff, TRUE, 0);
    }

  return TRUE;
}

static gboolean
get_checksum_from_deployment (OstreeRepo *repo, OstreeDeployment *deployment, char **out_checksum,
                              GError **error)
{
  g_autofree char *checksum = NULL;
  if (opt_base)
    {
      if (!rpmostree_deployment_get_base_layer (repo, deployment, &checksum, error))
        return FALSE;
    }

  if (!checksum)
    checksum = g_strdup (ostree_deployment_get_csum (deployment));

  *out_checksum = util::move_nullify (checksum);
  return TRUE;
}

static gboolean
print_deployment_diff (OstreeRepo *repo, const char *from_desc, OstreeDeployment *from,
                       const char *to_desc, OstreeDeployment *to, GCancellable *cancellable,
                       GError **error)
{
  g_autofree char *from_checksum = NULL;
  if (!get_checksum_from_deployment (repo, from, &from_checksum, error))
    return FALSE;

  g_autofree char *to_checksum = NULL;
  if (!get_checksum_from_deployment (repo, to, &to_checksum, error))
    return FALSE;

  return print_diff (repo, from_desc, from_checksum, to_desc, to_checksum, cancellable, error);
}

gboolean
rpmostree_db_builtin_diff (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("[FROM_REV] [TO_REV]");

  g_autoptr (OstreeRepo) repo = NULL;
  if (!rpmostree_db_option_context_parse (context, option_entries, &argc, &argv, invocation, &repo,
                                          cancellable, error))
    return FALSE;

  if (argc > 3)
    {
      g_autofree char *message
          = g_strdup_printf ("\"%s\" takes at most 2 arguments", g_get_prgname ());
      rpmostree_usage_error (context, message, error);
      return FALSE;
    }

  if (!opt_format)
    opt_format = g_strdup ("block");

  if (g_str_equal (opt_format, "json") && opt_changelogs)
    {
      rpmostree_usage_error (context, "json format and --changelogs not supported", error);
      return FALSE;
    }

  if (g_str_equal (opt_format, "diff") && opt_advisories)
    {
      rpmostree_usage_error (context, "diff format and --advisories not supported", error);
      return FALSE;
    }

  if (!g_str_equal (opt_format, "block") && !g_str_equal (opt_format, "diff")
      && !g_str_equal (opt_format, "json"))
    {
      rpmostree_usage_error (context, "Format argument is invalid, pick one of: diff, block, json",
                             error);
      return FALSE;
    }

  const char *from_desc = NULL;
  g_autofree char *from_checksum = NULL;
  const char *to_desc = NULL;
  g_autofree char *to_checksum = NULL;

  if (argc < 3)
    {
      /* find booted deployment */
      const char *sysroot_path = opt_sysroot ?: "/";
      g_autoptr (GFile) sysroot_file = g_file_new_for_path (sysroot_path);
      g_autoptr (OstreeSysroot) sysroot = ostree_sysroot_new (sysroot_file);
      if (!ostree_sysroot_load (sysroot, cancellable, error))
        return FALSE;

      OstreeDeployment *booted = ostree_sysroot_get_booted_deployment (sysroot);
      if (!booted)
        return glnx_throw (error, "Not booted into any deployment");

      if (argc < 2)
        {
          /* diff booted against pending or rollback deployment */
          g_autoptr (OstreeDeployment) pending = NULL;
          g_autoptr (OstreeDeployment) rollback = NULL;
          ostree_sysroot_query_deployments_for (sysroot, NULL, &pending, &rollback);
          if (pending)
            {
              return print_deployment_diff (repo, "booted deployment", booted, "pending deployment",
                                            pending, cancellable, error);
            }
          if (rollback)
            {
              return print_deployment_diff (repo, "rollback deployment", rollback,
                                            "booted deployment", booted, cancellable, error);
            }
          return glnx_throw (error, "No pending or rollback deployment to diff against");
        }
      else
        {
          from_desc = "booted deployment";
          if (!get_checksum_from_deployment (repo, booted, &from_checksum, error))
            return FALSE;

          /* diff against the booted deployment */
          to_desc = argv[1];
          if (!ostree_repo_resolve_rev (repo, to_desc, FALSE, &to_checksum, error))
            return FALSE;
        }
    }
  else
    {
      from_desc = argv[1];
      if (!ostree_repo_resolve_rev (repo, from_desc, FALSE, &from_checksum, error))
        return FALSE;

      to_desc = argv[2];
      if (!ostree_repo_resolve_rev (repo, to_desc, FALSE, &to_checksum, error))
        return FALSE;
    }

  if (g_str_equal (opt_format, "json"))
    {
      g_auto (GVariantBuilder) builder;
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&builder, "{sv}", "ostree-commit-from",
                             g_variant_new_string (from_checksum));
      g_variant_builder_add (&builder, "{sv}", "ostree-commit-to",
                             g_variant_new_string (to_checksum));

      g_autoptr (GVariant) diffv = NULL;
      if (!rpm_ostree_db_diff_variant (repo, from_checksum, to_checksum, FALSE, &diffv, cancellable,
                                       error))
        return FALSE;
      g_variant_builder_add (&builder, "{sv}", "pkgdiff", diffv);
      auto adv_diff
          = CXX_TRY_VAL (calculate_advisories_diff (*repo, from_checksum, to_checksum), error);
      g_variant_builder_add (&builder, "{sv}", "advisories", adv_diff);
      g_autoptr (GVariant) metadata = g_variant_builder_end (&builder);

      JsonNode *node = json_gvariant_serialize (metadata);
      glnx_unref_object JsonGenerator *generator = json_generator_new ();
      json_generator_set_pretty (generator, TRUE);
      json_generator_set_root (generator, node);

      glnx_unref_object GOutputStream *stdout_gio = g_unix_output_stream_new (1, FALSE);
      /* NB: watch out for the misleading API docs */
      if (json_generator_to_stream (generator, stdout_gio, cancellable, error) <= 0
          || (error != NULL && *error != NULL))
        return FALSE;

      return TRUE;
    }

  return print_diff (repo, from_desc, from_checksum, to_desc, to_checksum, cancellable, error);
}
