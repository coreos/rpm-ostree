/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <libglnx.h>
#include <systemd/sd-journal.h>

#include "rpmostree-core.h"
#include "rpmostree-cxxrs.h"
#include "rpmostree-origin.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-sysroot-core.h"
#include "rpmostree-types.h"
#include "rpmostree-util.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-errors.h"
#include "rpmostreed-utils.h"

/* rpmostreed_deployment_get_for_index:
 *
 * sysroot: A #OstreeSysroot instance
 * index: string index being parsed
 * error: a #GError instance
 *
 * Get a deployment based on a string index,
 * the string is parsed and checked. Then
 * the deployment at the parsed index will be
 * returned.
 *
 * returns: NULL if an error is being made
 */
OstreeDeployment *
rpmostreed_deployment_get_for_index (OstreeSysroot *sysroot, const gchar *index, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  int deployment_index = -1;
  for (int counter = 0; counter < strlen (index); counter++)
    {
      if (!g_ascii_isdigit (index[counter]))
        {
          local_error
              = g_error_new (RPM_OSTREED_ERROR, RPM_OSTREED_ERROR_FAILED,
                             "Invalid deployment index %s, must be a number and >= 0", index);
          g_propagate_error (error, util::move_nullify (local_error));
          return NULL;
        }
    }
  deployment_index = atoi (index);

  g_autoptr (GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployment_index >= deployments->len)
    {
      local_error = g_error_new (RPM_OSTREED_ERROR, RPM_OSTREED_ERROR_FAILED,
                                 "Out of range deployment index %d, expected < %d",
                                 deployment_index, deployments->len);
      g_propagate_error (error, util::move_nullify (local_error));
      return NULL;
    }
  return (OstreeDeployment *)g_object_ref (deployments->pdata[deployment_index]);
}

GVariant *
rpmostreed_deployment_generate_blank_variant (void)
{
  GVariantDict dict;

  g_variant_dict_init (&dict, NULL);

  return g_variant_dict_end (&dict);
}

static void
variant_add_metadata_attribute (GVariantDict *dict, const gchar *attribute,
                                const gchar *new_attribute, GVariant *commit)
{
  g_autofree gchar *attribute_string_value = NULL;
  g_autoptr (GVariant) metadata = g_variant_get_child_value (commit, 0);

  if (metadata != NULL)
    {
      g_variant_lookup (metadata, attribute, "s", &attribute_string_value);
      if (attribute_string_value != NULL)
        g_variant_dict_insert (dict, new_attribute ?: attribute, "s", attribute_string_value);
    }
}

static void
variant_add_commit_details (GVariantDict *dict, const char *prefix, GVariant *commit)
{
  g_autoptr (GVariant) metadata = NULL;
  g_autofree gchar *version_commit = NULL;
  guint64 timestamp = 0;

  timestamp = ostree_commit_get_timestamp (commit);
  metadata = g_variant_get_child_value (commit, 0);
  if (metadata != NULL)
    g_variant_lookup (metadata, "version", "s", &version_commit);

  if (version_commit != NULL)
    g_variant_dict_insert (dict, glnx_strjoina (prefix ?: "", "version"), "s", version_commit);
  if (timestamp > 0)
    g_variant_dict_insert (dict, glnx_strjoina (prefix ?: "", "timestamp"), "t", timestamp);
}

/* Returns floating GVariant equivalent of `commit_meta` minus keys we don't want. */
static GVariant *
filter_commit_meta (GVariant *commit_meta)
{
  GVariantDict dict;
  g_variant_dict_init (&dict, commit_meta);
  /* just remove keys for now, later we may want to define a specific list of keys to keep */
  g_variant_dict_remove (&dict, "rpmostree.rpmdb.pkglist");
  g_variant_dict_remove (&dict, "rpmostree.advisories");
  /* these layered commit keys are already promoted up, so just remove them to avoid duplicating and
   * because they can get quite verbose */
  g_variant_dict_remove (&dict, "rpmostree.packages"); /* promoted as 'packages' */
  g_variant_dict_remove (&dict, "rpmostree.modules");  /* promoted as 'modules' */
  g_variant_dict_remove (&dict, "rpmostree.spec");     /* old way of getting packages */
  g_variant_dict_remove (&dict, "rpmostree.removed-base-packages");  /* 'base-removals' */
  g_variant_dict_remove (&dict, "rpmostree.replaced-base-packages"); /* 'base-local-replacements' */
  g_variant_dict_remove (
      &dict, "rpmostree.replaced-base-remote-packages"); /* 'base-remote-replacements' */
  return g_variant_dict_end (&dict);
}

gboolean
rpmostreed_deployment_generate_variant (OstreeSysroot *sysroot, OstreeDeployment *deployment,
                                        const char *booted_id, OstreeRepo *repo, gboolean filter,
                                        GVariant **out_variant, GError **error)
{
  g_autoptr (GVariantDict) dict = g_variant_dict_new (NULL);

  ROSCXX_TRY (deployment_populate_variant (*sysroot, *deployment, *dict), error);
  const gchar *csum = ostree_deployment_get_csum (deployment);
  /* Load the commit object */
  g_autoptr (GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, csum, &commit, error))
    return FALSE;

  /* And the origin */
  g_autoptr (RpmOstreeOrigin) origin = rpmostree_origin_parse_deployment (deployment, error);
  if (!origin)
    return FALSE;

  auto r = rpmostree_origin_get_refspec (origin);
  const char *refspec = r.refspec.c_str ();

  gboolean is_layered = FALSE;
  g_autofree char *base_checksum = NULL;
  g_auto (GStrv) layered_pkgs = NULL;
  g_auto (GStrv) layered_modules = NULL;
  g_autoptr (GVariant) removed_base_pkgs = NULL;
  g_autoptr (GVariant) replaced_base_local_pkgs = NULL;
  g_autoptr (GVariant) replaced_base_remote_pkgs = NULL;
  if (!rpmostree_deployment_get_layered_info (
          repo, deployment, &is_layered, NULL, &base_checksum, &layered_pkgs, &layered_modules,
          &removed_base_pkgs, &replaced_base_local_pkgs, &replaced_base_remote_pkgs, error))
    return FALSE;

  g_autoptr (GVariant) base_commit = NULL;
  if (is_layered)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, base_checksum, &base_commit,
                                     error))
        return FALSE;

      g_variant_dict_insert (dict, "base-checksum", "s", base_checksum);
      variant_add_commit_details (dict, "base-", base_commit);
      /* for layered commits, check if their base commit has end of life attribute */
      variant_add_metadata_attribute (dict, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "endoflife",
                                      base_commit);

      /* See below for base commit metadata */
      g_autoptr (GVariant) layered_metadata = g_variant_get_child_value (commit, 0);
      g_variant_dict_insert (dict, "layered-commit-meta", "@a{sv}",
                             filter ? filter_commit_meta (layered_metadata) : layered_metadata);
    }
  else
    {
      base_commit = g_variant_ref (commit);
      base_checksum = g_strdup (csum);
      variant_add_metadata_attribute (dict, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "endoflife", commit);
    }

  /* We used to bridge individual keys, but that was annoying; just pass through all
   * of the commit metadata that are actually relevant.
   */
  {
    g_autoptr (GVariant) base_meta = g_variant_get_child_value (base_commit, 0);
    g_variant_dict_insert (dict, "base-commit-meta", "@a{sv}",
                           filter ? filter_commit_meta (base_meta) : base_meta);
  }
  variant_add_commit_details (dict, NULL, commit);

  switch (r.kind)
    {
    case rpmostreecxx::RefspecType::Container:
      {
        g_variant_dict_insert (dict, "container-image-reference", "s", refspec);
        // For now, make this non-fatal https://github.com/coreos/rpm-ostree/issues/4185
        try
          {
            auto state = rpmostreecxx::query_container_image_commit (*repo, base_checksum);
            g_variant_dict_insert (dict, "container-image-reference-digest", "s",
                                   state->image_digest.c_str ());
            if (state->version.size () > 0)
              g_variant_dict_insert (dict, "version", "s", state->version.c_str ());
          }
        catch (std::exception &e)
          {
            sd_journal_print (LOG_ERR, "failed to query container image base metadata: %s",
                              e.what ());
          }
      }
      break;
    case rpmostreecxx::RefspecType::Checksum:
      {
        g_variant_dict_insert (dict, "origin", "s", refspec);
        auto custom_origin_url = rpmostree_origin_get_custom_url (origin);
        auto custom_origin_description = rpmostree_origin_get_custom_description (origin);
        if (!custom_origin_url.empty ())
          g_variant_dict_insert (dict, "custom-origin", "(ss)", custom_origin_url.c_str (),
                                 custom_origin_description.c_str ());
      }
      break;
    case rpmostreecxx::RefspecType::Ostree:
      {
        g_variant_dict_insert (dict, "origin", "s", refspec);
        ROSCXX_TRY (variant_add_remote_status (*repo, refspec, base_checksum, *dict), error);

        g_autofree char *pending_base_commitrev = NULL;
        if (!ostree_repo_resolve_rev (repo, r.refspec.c_str (), TRUE, &pending_base_commitrev,
                                      error))
          return FALSE;

        if (pending_base_commitrev && !g_str_equal (pending_base_commitrev, base_checksum))
          {
            g_autoptr (GVariant) pending_base_commit = NULL;

            if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, pending_base_commitrev,
                                           &pending_base_commit, error))
              return FALSE;

            g_variant_dict_insert (dict, "pending-base-checksum", "s", pending_base_commitrev);
            variant_add_commit_details (dict, "pending-base-", pending_base_commit);
          }
      }
      break;
    }

  g_variant_dict_insert (dict, "packages", "^as", layered_pkgs);
  g_variant_dict_insert (dict, "modules", "^as", layered_modules);
  g_variant_dict_insert_value (dict, "base-removals", removed_base_pkgs);
  g_variant_dict_insert_value (dict, "base-local-replacements", replaced_base_local_pkgs);
  g_variant_dict_insert_value (dict, "base-remote-replacements", replaced_base_remote_pkgs);

  *out_variant = g_variant_dict_end (dict);
  return TRUE;
}

/* Adds the following keys to the vardict:
 *  - osname
 *  - checksum
 *  - version
 *  - timestamp
 *  - origin
 *  - signatures
 *  - gpg-enabled
 *
 * If not %NULL, @refspec and @checksum override defaults from @deployment. They can also be
 * used to avoid lookups if they're already available.
 * */
static gboolean
add_all_commit_details_to_vardict (OstreeDeployment *deployment, OstreeRepo *repo,
                                   const char *refspec,  /* allow-none */
                                   const char *checksum, /* allow-none */
                                   GVariant *commit,     /* allow-none */
                                   GVariantDict *dict,   /* allow-none */
                                   GError **error)
{
  const gchar *osname = ostree_deployment_get_osname (deployment);

  rpmostreecxx::Refspec refspec_owned;
  gboolean refspec_is_ostree = FALSE;
  if (!refspec)
    {
      g_autoptr (RpmOstreeOrigin) origin = rpmostree_origin_parse_deployment (deployment, error);
      if (!origin)
        return FALSE;
      refspec_owned = rpmostree_origin_get_refspec (origin);
      refspec = refspec_owned.refspec.c_str ();
    }
  auto refspec_type = rpmostreecxx::refspec_classify (refspec);

  (void)refspec_owned; /* Pacify static analysis */
  refspec_is_ostree = refspec_type == rpmostreecxx::RefspecType::Ostree;
  if (refspec_type == rpmostreecxx::RefspecType::Checksum && !commit)
    checksum = refspec;

  g_assert (refspec);

  /* allow_noent=TRUE since the ref may have been deleted for a rebase */
  g_autofree gchar *checksum_owned = NULL;
  if (!checksum)
    {
      if (refspec_is_ostree)
        {
          if (!ostree_repo_resolve_rev (repo, refspec, TRUE, &checksum_owned, error))
            return FALSE;
        }

      /* in that case, use deployment csum */
      checksum = checksum_owned ?: ostree_deployment_get_csum (deployment);
    }

  g_autoptr (GVariant) commit_owned = NULL;
  if (!commit)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum, &commit_owned,
                                     error))
        return FALSE;
      commit = commit_owned;
    }

  if (refspec_is_ostree)
    {
      ROSCXX_TRY (variant_add_remote_status (*repo, refspec, checksum, *dict), error);
    }

  if (osname != NULL)
    g_variant_dict_insert (dict, "osname", "s", osname);
  g_variant_dict_insert (dict, "checksum", "s", checksum);
  variant_add_commit_details (dict, NULL, commit);
  g_variant_dict_insert (dict, "origin", "s", refspec);
  return TRUE;
}

GVariant *
rpmostreed_commit_generate_cached_details_variant (OstreeDeployment *deployment, OstreeRepo *repo,
                                                   const char *refspec, const char *checksum,
                                                   GError **error)
{
  GVariantDict dict;
  g_variant_dict_init (&dict, NULL);
  if (!add_all_commit_details_to_vardict (deployment, repo, refspec, checksum, NULL, &dict, error))
    return NULL;

  return g_variant_ref_sink (g_variant_dict_end (&dict));
}

typedef struct
{
  gboolean initialized;
  GPtrArray *upgraded;
  GPtrArray *downgraded;
  GPtrArray *removed;
  GPtrArray *added;
} RpmDiff;

static void
rpm_diff_init (RpmDiff *diff)
{
  g_assert (!diff->initialized);
  diff->upgraded = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  diff->downgraded = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  diff->removed = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  diff->added = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  diff->initialized = TRUE;
}

static void
rpm_diff_clear (RpmDiff *diff)
{
  if (!diff->initialized)
    return;
  g_clear_pointer (&diff->upgraded, (GDestroyNotify)g_ptr_array_unref);
  g_clear_pointer (&diff->downgraded, (GDestroyNotify)g_ptr_array_unref);
  g_clear_pointer (&diff->removed, (GDestroyNotify)g_ptr_array_unref);
  g_clear_pointer (&diff->added, (GDestroyNotify)g_ptr_array_unref);
  diff->initialized = FALSE;
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (RpmDiff, rpm_diff_clear);

static GVariant *
single_pkg_variant_new (RpmOstreePkgTypes type, RpmOstreePackage *pkg)
{
  return g_variant_ref_sink (g_variant_new ("(usss)", type, rpm_ostree_package_get_name (pkg),
                                            rpm_ostree_package_get_evr (pkg),
                                            rpm_ostree_package_get_arch (pkg)));
}

static GVariant *
modified_pkg_variant_new (RpmOstreePkgTypes type, RpmOstreePackage *pkg_old,
                          RpmOstreePackage *pkg_new)
{
  const char *name_old = rpm_ostree_package_get_name (pkg_old);
  const char *name_new = rpm_ostree_package_get_name (pkg_new);
  g_assert_cmpstr (name_old, ==, name_new);
  return g_variant_ref_sink (
      g_variant_new ("(us(ss)(ss))", type, name_old, rpm_ostree_package_get_evr (pkg_old),
                     rpm_ostree_package_get_arch (pkg_old), rpm_ostree_package_get_evr (pkg_new),
                     rpm_ostree_package_get_arch (pkg_new)));
}

static GVariant *
modified_dnfpkg_variant_new (RpmOstreePkgTypes type, RpmOstreePackage *pkg_old, DnfPackage *pkg_new)
{
  const char *name_old = rpm_ostree_package_get_name (pkg_old);
  const char *name_new = dnf_package_get_name (pkg_new);
  g_assert_cmpstr (name_old, ==, name_new);
  return g_variant_ref_sink (
      g_variant_new ("(us(ss)(ss))", type, name_old, rpm_ostree_package_get_evr (pkg_old),
                     rpm_ostree_package_get_arch (pkg_old), dnf_package_get_evr (pkg_new),
                     dnf_package_get_arch (pkg_new)));
}

static gboolean
rpm_diff_add_db_diff (RpmDiff *diff, OstreeRepo *repo, RpmOstreePkgTypes type,
                      const char *old_checksum, const char *new_checksum,
                      GPtrArray **out_modified_new, GCancellable *cancellable, GError **error)
{
  g_autoptr (GPtrArray) removed = NULL;
  g_autoptr (GPtrArray) added = NULL;
  g_autoptr (GPtrArray) modified_old = NULL;
  g_autoptr (GPtrArray) modified_new = NULL;

  /* Use allow_noent; we'll just skip over the rpm diff if there's no data */
  RpmOstreeDbDiffExtFlags flags = RPM_OSTREE_DB_DIFF_EXT_ALLOW_NOENT;
  if (!rpm_ostree_db_diff_ext (repo, old_checksum, new_checksum, flags, &removed, &added,
                               &modified_old, &modified_new, cancellable, error))
    return FALSE;

  /* check if allow_noent kicked in */
  if (!removed)
    return TRUE; /* NB: early return */

  g_assert_cmpuint (modified_old->len, ==, modified_new->len);
  for (guint i = 0; i < removed->len; i++)
    g_ptr_array_add (diff->removed, single_pkg_variant_new (
                                        type, static_cast<RpmOstreePackage *> (removed->pdata[i])));
  for (guint i = 0; i < added->len; i++)
    g_ptr_array_add (diff->added, single_pkg_variant_new (
                                      type, static_cast<RpmOstreePackage *> (added->pdata[i])));
  for (guint i = 0; i < modified_old->len; i++)
    {
      auto old_pkg = static_cast<RpmOstreePackage *> (modified_old->pdata[i]);
      auto new_pkg = static_cast<RpmOstreePackage *> (modified_new->pdata[i]);
      if (rpm_ostree_package_cmp (old_pkg, new_pkg) < 0)
        g_ptr_array_add (diff->upgraded, modified_pkg_variant_new (type, old_pkg, new_pkg));
      else
        g_ptr_array_add (diff->downgraded, modified_pkg_variant_new (type, old_pkg, new_pkg));
    }

  if (out_modified_new)
    *out_modified_new = util::move_nullify (modified_new);
  return TRUE;
}

static void
rpm_diff_add_layered_diff (RpmDiff *diff, RpmOstreePackage *old_pkg, DnfPackage *new_pkg)
{
  /* add to upgraded; layered pkgs only go up */
  RpmOstreePkgTypes type = RPM_OSTREE_PKG_TYPE_LAYER;
  g_ptr_array_add (diff->upgraded, modified_dnfpkg_variant_new (type, old_pkg, new_pkg));
}

static int
sort_pkgvariant_by_name (gconstpointer pkga_pp, gconstpointer pkgb_pp)
{
  GVariant *pkg_a = *((GVariant **)pkga_pp);
  GVariant *pkg_b = *((GVariant **)pkgb_pp);

  const char *pkgname_a;
  g_variant_get_child (pkg_a, 1, "&s", &pkgname_a);
  const char *pkgname_b;
  g_variant_get_child (pkg_b, 1, "&s", &pkgname_b);

  return strcmp (pkgname_a, pkgname_b);
}

static GVariant *
array_to_variant_new (const char *format, GPtrArray *array)
{
  if (array->len == 0)
    return g_variant_new (format, NULL);

  /* make doubly sure it's sorted */
  g_ptr_array_sort (array, sort_pkgvariant_by_name);

  g_auto (GVariantBuilder) builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE (format));
  for (guint i = 0; i < array->len; i++)
    g_variant_builder_add_value (&builder, static_cast<GVariant *> (array->pdata[i]));
  return g_variant_builder_end (&builder);
}

static gboolean
rpm_diff_is_empty (RpmDiff *diff)
{
  g_assert (diff->initialized);
  return !diff->upgraded->len && !diff->downgraded->len && !diff->removed->len && !diff->added->len;
}

static GVariant *
rpm_diff_variant_new (RpmDiff *diff)
{
  g_assert (diff->initialized);
  g_auto (GVariantDict) dict;
  g_variant_dict_init (&dict, NULL);
  g_variant_dict_insert_value (
      &dict, "upgraded",
      array_to_variant_new (RPMOSTREE_DIFF_MODIFIED_GVARIANT_STRING, diff->upgraded));
  g_variant_dict_insert_value (
      &dict, "downgraded",
      array_to_variant_new (RPMOSTREE_DIFF_MODIFIED_GVARIANT_STRING, diff->downgraded));
  g_variant_dict_insert_value (
      &dict, "removed",
      array_to_variant_new (RPMOSTREE_DIFF_SINGLE_GVARIANT_STRING, diff->removed));
  g_variant_dict_insert_value (
      &dict, "added", array_to_variant_new (RPMOSTREE_DIFF_SINGLE_GVARIANT_STRING, diff->added));
  return g_variant_dict_end (&dict);
}

static DnfPackage *
find_package (DnfSack *sack, gboolean newer, RpmOstreePackage *pkg)
{
  hy_autoquery HyQuery query = hy_query_create (sack);
  hy_query_filter (query, HY_PKG_NAME, HY_EQ, rpm_ostree_package_get_name (pkg));
  if (newer)
    {
      hy_query_filter (query, HY_PKG_EVR, HY_GT, rpm_ostree_package_get_evr (pkg));
      hy_query_filter (query, HY_PKG_ARCH, HY_NEQ, "src");
      hy_query_filter_latest (query, TRUE);
    }
  else
    {
      /* we want an exact match */
      hy_query_filter (query, HY_PKG_NEVRA, HY_EQ, rpm_ostree_package_get_nevra (pkg));
    }
  g_autoptr (GPtrArray) pkgs = hy_query_run (query);
  if (pkgs->len == 0)
    return NULL; /* canonicalize to NULL */
  g_ptr_array_sort (pkgs, (GCompareFunc)rpmostree_pkg_array_compare);
  return (DnfPackage *)g_object_ref (pkgs->pdata[pkgs->len - 1]);
}

/* For all layered pkgs, check if there are newer versions in the rpmmd. Add diff to
 * @rpm_diff, and all new pkgs in @out_newer_packages (these are used later for advisories).
 * */
static gboolean
rpmmd_diff_guess (OstreeRepo *repo, const char *base_checksum, const char *layered_checksum,
                  DnfSack *sack, RpmDiff *rpm_diff, GPtrArray **out_newer_packages, GError **error)
{
  /* Note here that we *don't* actually use layered_pkgs; we want to look at all the RPMs
   * installed, whereas the layered pkgs (actually patterns) just represent top-level
   * entries. IOW, we want to run through all layered RPMs, which include deps of
   * layered_pkgs. */

  g_autoptr (GPtrArray) all_layered_pkgs = NULL;
  RpmOstreeDbDiffExtFlags flags = RPM_OSTREE_DB_DIFF_EXT_ALLOW_NOENT;
  if (!rpm_ostree_db_diff_ext (repo, base_checksum, layered_checksum, flags, NULL,
                               &all_layered_pkgs, NULL, NULL, NULL, error))
    return FALSE;

  /* XXX: need to filter out local pkgs; though we still want to check for advisories --
   * maybe we should do this in status.c instead? */

  if (all_layered_pkgs == NULL || /* -> older layer before we injected pkglist metadata */
      all_layered_pkgs->len == 0) /* -> no layered pkgs, e.g. override remove only */
    {
      *out_newer_packages = NULL;
      return TRUE; /* note early return */
    }

  /* for each layered pkg, check if there's a newer version available (in reality, there may
   * be other new pkgs that need to be layered or some pkgs that no longer need to, but we
   * won't find out until we have the full commit available -- XXX: we could go the extra
   * effort and use the rpmdb of new_checksum if we already have it somehow, though that's
   * probably not the common case */

  g_autoptr (GPtrArray) newer_packages
      = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  for (guint i = 0; i < all_layered_pkgs->len; i++)
    {
      auto pkg = static_cast<RpmOstreePackage *> (all_layered_pkgs->pdata[i]);
      g_autoptr (DnfPackage) newer_pkg = find_package (sack, TRUE, pkg);
      if (!newer_pkg)
        continue;

      g_ptr_array_add (newer_packages, g_object_ref (newer_pkg));
      rpm_diff_add_layered_diff (rpm_diff, pkg, newer_pkg);
    }

  /* canonicalize to NULL if there's nothing new */
  if (newer_packages->len == 0)
    g_clear_pointer (&newer_packages, (GDestroyNotify)g_ptr_array_unref);

  *out_newer_packages = util::move_nullify (newer_packages);
  return TRUE;
}

/* try to find the exact same RpmOstreePackage pkgs in the sack */
static GPtrArray *
rpm_ostree_pkgs_to_dnf (DnfSack *sack, GPtrArray *rpm_ostree_pkgs)
{
  g_autoptr (GPtrArray) dnf_pkgs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  const guint n = rpm_ostree_pkgs->len;
  for (guint i = 0; i < n; i++)
    {
      auto pkg = static_cast<RpmOstreePackage *> (rpm_ostree_pkgs->pdata[i]);
      hy_autoquery HyQuery query = hy_query_create (sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, rpm_ostree_package_get_name (pkg));
      hy_query_filter (query, HY_PKG_EVR, HY_EQ, rpm_ostree_package_get_evr (pkg));
      hy_query_filter (query, HY_PKG_ARCH, HY_EQ, rpm_ostree_package_get_arch (pkg));
      g_autoptr (GPtrArray) pkgs = hy_query_run (query);

      /* 0 --> ostree stream is out of sync with rpmmd repos probably? */
      if (pkgs->len > 0)
        g_ptr_array_add (dnf_pkgs, g_object_ref (pkgs->pdata[0]));
    }

  return util::move_nullify (dnf_pkgs);
}

/* The variant returned by this function is backwards compatible with the one returned by
 * rpmostreed_commit_generate_cached_details_variant(). However, it also includes a base
 * tree db diff, layered pkgs diff, state, advisories, etc... Also, it will happily return
 * %NULL if no updates are available.
 *
 * If @staged_deployment is %NULL, update details are based on latest downloaded ostree
 * rpmmd metadata. If @staged_deployment is not %NULL, then the update describes the diff
 * between @booted_deployment and @staged_deployment. */
gboolean
rpmostreed_update_generate_variant (OstreeDeployment *booted_deployment,
                                    OstreeDeployment *staged_deployment, OstreeRepo *repo,
                                    DnfSack *sack, /* allow-none */
                                    GVariant **out_update, GCancellable *cancellable,
                                    GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Generating update variant", error);

  g_autoptr (RpmOstreeOrigin) origin = rpmostree_origin_parse_deployment (booted_deployment, error);
  if (!origin)
    return FALSE;

  auto r = rpmostree_origin_get_refspec (origin);

  /* let's start with the ostree side of things */

  const char *current_checksum = ostree_deployment_get_csum (booted_deployment);
  g_autofree char *current_base_checksum_owned = NULL;
  if (!rpmostree_deployment_get_base_layer (repo, booted_deployment, &current_base_checksum_owned,
                                            error))
    return FALSE;
  const char *current_base_checksum = current_base_checksum_owned ?: current_checksum;

  gboolean is_new_layered = FALSE;
  const char *new_checksum = NULL;
  const char *new_base_checksum = NULL;
  g_autofree char *new_base_checksum_owned = NULL;
  auto refspectype = rpmostreecxx::refspec_classify (r.refspec);

  if (staged_deployment)
    {
      new_checksum = ostree_deployment_get_csum (staged_deployment);
      if (!rpmostree_deployment_get_base_layer (repo, staged_deployment, &new_base_checksum_owned,
                                                error))
        return FALSE;
      new_base_checksum = new_base_checksum_owned ?: new_checksum;
    }
  else
    {
      if (refspectype != rpmostreecxx::RefspecType::Container)
        {
          if (!ostree_repo_resolve_rev_ext (repo, r.refspec.c_str (), TRUE,
                                            static_cast<OstreeRepoResolveRevExtFlags> (0),
                                            &new_base_checksum_owned, error))
            {
              return FALSE;
            }
          new_base_checksum = new_base_checksum_owned;
          /* just assume that the hypothetical new deployment would also be layered if we are */
          is_new_layered = (current_base_checksum_owned != NULL);
        }
    }

  /* Graciously handle rev no longer in repo; e.g. mucking around with rebase/rollback; we
   * still want to do the rpm-md phase. In that case, just use the current csum. */
  gboolean is_new_checksum = FALSE;
  if (!new_base_checksum)
    new_base_checksum = current_base_checksum;
  else
    is_new_checksum = !g_str_equal (new_base_checksum, current_base_checksum);

  g_autoptr (GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, new_base_checksum, &commit, NULL, error))
    return FALSE;

  g_autoptr (GVariantDict) dict = g_variant_dict_new (NULL);

  /* first get all the traditional/backcompat stuff */
  if (!add_all_commit_details_to_vardict (booted_deployment, repo, r.refspec.c_str (),
                                          new_base_checksum, commit, dict, error))
    return FALSE;

  /* This may seem trivial, but it's important to keep the final variant as self-contained
   * and "diff-based" as possible, since it'll be available as a D-Bus property. This makes
   * it easier to consume for UIs like GNOME Software and Cockpit. */
  g_variant_dict_insert (dict, "ref-has-new-commit", "b", is_new_checksum);

  g_auto (RpmDiff) rpm_diff = {
    0,
  };
  rpm_diff_init (&rpm_diff);

  g_autoptr (GPtrArray) ostree_modified_new = NULL;
  g_autoptr (GPtrArray) rpmmd_modified_new = NULL;

  bool container_changed = false;

  if (staged_deployment)
    {
      g_debug ("Computing diff with staged deployment");

      /* ok we have a staged deployment; we just need to do a simple diff and BOOM done! */
      /* XXX: we're marking all pkgs as BASE right now even though there could be layered
       * pkgs too -- we can tease those out in the future if needed */
      if (!rpm_diff_add_db_diff (&rpm_diff, repo, RPM_OSTREE_PKG_TYPE_BASE, current_checksum,
                                 new_checksum, &ostree_modified_new, cancellable, error))
        return FALSE;
    }
  else
    {
      /* no staged deployment; we do our best to come up with a diff:
       *  - if a new base checksum was pulled, do a db diff of the old and new bases
       *  - if there are currently any layered pkgs, lookup in sack for newer versions
       */

      if (is_new_checksum && refspectype != rpmostreecxx::RefspecType::Container)
        {
          if (!rpm_diff_add_db_diff (&rpm_diff, repo, RPM_OSTREE_PKG_TYPE_BASE,
                                     current_base_checksum, new_base_checksum, &ostree_modified_new,
                                     cancellable, error))
            {
              return FALSE;
            }
        }

      /* now we look at the rpm-md/layering side */
      g_autofree char *origin_remote = NULL;
      g_autofree char *origin_ref = NULL;

      if (refspectype != rpmostreecxx::RefspecType::Container)
        {
          if (!ostree_parse_refspec (r.refspec.c_str (), &origin_remote, &origin_ref, error))
            return FALSE;
        }
      else
        {
          // Make this an operation we allow to fail, see the other call
          try
            {
              auto state = rpmostreecxx::query_container_image_commit (*repo, current_checksum);
              container_changed
                  = rpmostreecxx::deployment_add_manifest_diff (*dict, state->cached_update_diff);
              g_debug ("container changed: %d", container_changed);
            }
          catch (std::exception &e)
            {
              sd_journal_print (LOG_ERR, "failed to query container image base metadata: %s",
                                e.what ());
            }
        }

      /* check that it's actually layered (i.e. the requests are not all just dormant) */
      if (sack && is_new_layered && rpmostree_origin_has_packages (origin))
        {
          if (!rpmmd_diff_guess (repo, current_base_checksum, current_checksum, sack, &rpm_diff,
                                 &rpmmd_modified_new, error))
            return FALSE;
        }
    }

  /* don't bother inserting if there's nothing new */
  if (!rpm_diff_is_empty (&rpm_diff))
    g_variant_dict_insert (dict, "rpm-diff", "@a{sv}", rpm_diff_variant_new (&rpm_diff));

  /* now we look for advisories */
  if (sack && (ostree_modified_new || rpmmd_modified_new))
    {
      /* let's just merge the two now for convenience */
      g_autoptr (GPtrArray) new_packages
          = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

      if (ostree_modified_new)
        {
          /* recall that @ostree_modified_new is an array of RpmOstreePackage; try to find
           * the same pkg in the rpmmd so that we can search for advisories afterwards */
          g_autoptr (GPtrArray) pkgs = rpm_ostree_pkgs_to_dnf (sack, ostree_modified_new);
          for (guint i = 0; i < pkgs->len; i++)
            g_ptr_array_add (new_packages, g_object_ref (pkgs->pdata[i]));
        }

      if (rpmmd_modified_new)
        {
          for (guint i = 0; i < rpmmd_modified_new->len; i++)
            g_ptr_array_add (new_packages, g_object_ref (rpmmd_modified_new->pdata[i]));
        }

      g_autoptr (GVariant) advisories = rpmostree_advisories_variant (sack, new_packages);
      if (advisories)
        g_variant_dict_insert (dict, "advisories", "@a(suuasa{sv})", advisories);
    }

  if (staged_deployment)
    {
      auto id = rpmostreecxx::deployment_generate_id (*staged_deployment);
      g_variant_dict_insert (dict, "deployment", "s", id.c_str ());
    }

  /* but if there are no updates, then just ditch the whole thing and return NULL */
  refspectype = rpmostreecxx::refspec_classify (r.refspec);
  if (is_new_checksum || rpmmd_modified_new || container_changed)
    {
      g_debug ("Recomputed cached update");
      /* include a "state" checksum for cache invalidation; for now this is just the
       * checksum of the deployment against which we ran, though we could base it off more
       * things later if needed */
      g_variant_dict_insert (dict, "update-sha256", "s", current_checksum);
      *out_update = g_variant_ref_sink (g_variant_dict_end (dict));
    }
  else
    {
      g_debug ("No cached update");
      *out_update = NULL;
    }

  return TRUE;
}
