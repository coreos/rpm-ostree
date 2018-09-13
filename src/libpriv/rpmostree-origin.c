/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

#include "string.h"

#include "libglnx.h"
#include "rpmostree-origin.h"
#include "rpmostree-core.h"
#include "rpmostree-util.h"
#include "rpmostree-rpm-util.h"

struct RpmOstreeOrigin {
  guint refcount;

  /* this is the single source of truth */
  GKeyFile *kf;

  /* An origin can have either a refspec or a rojigspec */
  RpmOstreeRefspecType refspec_type;
  char *cached_refspec;

  /* Version data that goes along with the refspec */
  char *cached_override_commit;
  char *cached_rojig_version;
  /* The NEVRA of the rojigRPM */
  char *cached_rojig_description;

  char *cached_unconfigured_state;
  char **cached_initramfs_args;
  GHashTable *cached_packages;                  /* set of reldeps */
  GHashTable *cached_local_packages;            /* NEVRA --> header sha256 */
  /* GHashTable *cached_overrides_replace;         XXX: NOT IMPLEMENTED YET */
  GHashTable *cached_overrides_local_replace;   /* NEVRA --> header sha256 */
  GHashTable *cached_overrides_remove;          /* set of pkgnames (no EVRA) */
};

static GKeyFile *
keyfile_dup (GKeyFile *kf)
{
  GKeyFile *ret = g_key_file_new ();
  gsize length = 0;
  g_autofree char *data = g_key_file_to_data (kf, &length, NULL);

  g_key_file_load_from_data (ret, data, length, G_KEY_FILE_KEEP_COMMENTS, NULL);
  return ret;
}

/* take <nevra:sha256> entries from keyfile and inserts them into hash table */
static gboolean
parse_packages_strv (GKeyFile *kf,
                     const char *group,
                     const char *key,
                     gboolean    has_sha256,
                     GHashTable *ht,
                     GError    **error)
{
  g_auto(GStrv) packages = g_key_file_get_string_list (kf, group, key, NULL, NULL);

  for (char **it = packages; it && *it; it++)
    {
      if (has_sha256)
        {
          const char *nevra = *it;
          g_autofree char *sha256 = NULL;
          if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", nevra);
          g_hash_table_replace (ht, g_strdup (nevra), g_steal_pointer (&sha256));
        }
      else
        {
          g_hash_table_add (ht, g_steal_pointer (it));
        }
    }

  return TRUE;
}

RpmOstreeOrigin *
rpmostree_origin_parse_keyfile (GKeyFile         *origin,
                                GError          **error)
{
  g_autoptr(RpmOstreeOrigin) ret = NULL;

  ret = g_new0 (RpmOstreeOrigin, 1);
  ret->refcount = 1;
  ret->kf = keyfile_dup (origin);

  ret->cached_packages = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  ret->cached_local_packages =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_overrides_local_replace =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ret->cached_overrides_remove =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  ret->cached_unconfigured_state = g_key_file_get_string (ret->kf, "origin", "unconfigured-state", NULL);

  g_autofree char *refspec = g_key_file_get_string (ret->kf, "origin", "refspec", NULL);
  g_autofree char *rojig_spec = g_key_file_get_string (ret->kf, "origin", "rojig", NULL);
  if (!refspec)
    {
      refspec = g_key_file_get_string (ret->kf, "origin", "baserefspec", NULL);
      if (!refspec && !rojig_spec)
        return glnx_null_throw (error, "No origin/refspec, origin/rojig, or origin/baserefspec in current deployment origin; cannot handle via rpm-ostree");
    }
  if (refspec && rojig_spec)
    return glnx_null_throw (error, "Duplicate origin/refspec and origin/rojig in deployment origin");
  else if (refspec)
    {
      if (!rpmostree_refspec_classify (refspec, &ret->refspec_type, NULL, error))
        return FALSE;
      /* Note the lack of a prefix here so that code that just calls
       * rpmostree_origin_get_refspec() in the ostree:// case
       * sees it without the prefix for compatibility.
       */
      ret->cached_refspec = g_steal_pointer (&refspec);
      ret->cached_override_commit =
        g_key_file_get_string (ret->kf, "origin", "override-commit", NULL);
    }
  else
    {
      g_assert (rojig_spec);
      ret->refspec_type = RPMOSTREE_REFSPEC_TYPE_ROJIG;
      ret->cached_refspec = g_steal_pointer (&rojig_spec);
      ret->cached_rojig_version = g_key_file_get_string (ret->kf, "origin", "rojig-override-version", NULL);
      ret->cached_rojig_description = g_key_file_get_string (ret->kf, "origin", "rojig-description", NULL);
    }

  if (!parse_packages_strv (ret->kf, "packages", "requested", FALSE,
                            ret->cached_packages, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "packages", "requested-local", TRUE,
                            ret->cached_local_packages, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "overrides", "remove", FALSE,
                            ret->cached_overrides_remove, error))
    return FALSE;

  if (!parse_packages_strv (ret->kf, "overrides", "replace-local", TRUE,
                            ret->cached_overrides_local_replace, error))
    return FALSE;

  ret->cached_initramfs_args =
    g_key_file_get_string_list (ret->kf, "rpmostree", "initramfs-args", NULL, NULL);

  return g_steal_pointer (&ret);
}

RpmOstreeOrigin *
rpmostree_origin_dup (RpmOstreeOrigin *origin)
{
  g_autoptr(GError) local_error = NULL;
  RpmOstreeOrigin *ret = rpmostree_origin_parse_keyfile (origin->kf, &local_error);
  g_assert_no_error (local_error);
  return ret;
}

/* This is useful if the origin is meant to be used to generate a *new* deployment, as
 * opposed to simply gathering information about an existing one. In such cases, there are
 * some things that we do not generally want to apply to a new deployment. */
void
rpmostree_origin_remove_transient_state (RpmOstreeOrigin *origin)
{
  /* first libostree-known things */
  ostree_deployment_origin_remove_transient_state (origin->kf);

  /* this is already covered by the above, but the below also updates the cached value */
  rpmostree_origin_set_override_commit (origin, NULL, NULL);

  /* then rpm-ostree specific things */
  rpmostree_origin_set_live_state (origin, NULL, NULL);
}

const char *
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin)
{
  return origin->cached_refspec;
}

/* For rojig:// refspecs, includes the prefix. */
char *
rpmostree_origin_get_full_refspec (RpmOstreeOrigin *origin,
                                   RpmOstreeRefspecType *out_refspectype)
{
  if (out_refspectype)
    *out_refspectype = origin->refspec_type;
  switch (origin->refspec_type)
    {
    case RPMOSTREE_REFSPEC_TYPE_OSTREE:
    case RPMOSTREE_REFSPEC_TYPE_CHECKSUM:
      return g_strdup (origin->cached_refspec);
    case RPMOSTREE_REFSPEC_TYPE_ROJIG:
      return g_strconcat (RPMOSTREE_REFSPEC_ROJIG_PREFIX, origin->cached_refspec, NULL);
    }
  g_assert_not_reached ();
  return NULL;
}

/* Returns: TRUE iff the origin type is rpm-ostree rojig */
gboolean
rpmostree_origin_is_rojig (RpmOstreeOrigin *origin)
{
  return origin->refspec_type == RPMOSTREE_REFSPEC_TYPE_ROJIG;
}

void
rpmostree_origin_classify_refspec (RpmOstreeOrigin      *origin,
                                   RpmOstreeRefspecType *out_type,
                                   const char          **out_refspecdata)
{
  *out_type = origin->refspec_type;
  if (out_refspecdata)
    *out_refspecdata = origin->cached_refspec;
}

const char *
rpmostree_origin_get_rojig_version (RpmOstreeOrigin *origin)
{
  return origin->cached_rojig_version;
}

/* Returns a new (floating) variant of type a{sv} with fields:
 *  - s: repo
 *  - s: name
 *  - s: evr
 *  - s: arch
 * Note this type is exposed as DBus API.
 */
GVariant *
rpmostree_origin_get_rojig_description (RpmOstreeOrigin *origin)
{
  const char *colon = strchr (origin->cached_refspec, ':');
  g_assert (colon);
  const char *repo = strndupa (origin->cached_refspec, colon - origin->cached_refspec);
  g_autofree char *rojig_evr = g_key_file_get_string (origin->kf, "origin", "rojig-imported-evr", NULL);
  g_autofree char *rojig_arch = g_key_file_get_string (origin->kf, "origin", "rojig-imported-arch", NULL);
  g_autoptr(GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (builder, "{sv}", "repo", g_variant_new_string (repo));
  g_variant_builder_add (builder, "{sv}", "name", g_variant_new_string (colon + 1));
  if (rojig_evr)
    g_variant_builder_add (builder, "{sv}", "evr", g_variant_new_string (rojig_evr));
  if (rojig_arch)
    g_variant_builder_add (builder, "{sv}", "arch", g_variant_new_string (rojig_arch));
  return g_variant_builder_end (builder);
}

static char *
keyfile_get_nonempty_string (GKeyFile *kf, const char *section, const char *key)
{
  char *ret = g_key_file_get_string (kf, section, key, NULL);
  if (ret && !*ret)
    g_clear_pointer (&ret, g_free);
  return ret;
}

void
rpmostree_origin_get_custom_description (RpmOstreeOrigin *origin,
                                         char           **custom_type,
                                         char           **custom_description)
{
  *custom_type = keyfile_get_nonempty_string (origin->kf, "origin", "custom-url");
  if (*custom_type)
    *custom_description = keyfile_get_nonempty_string (origin->kf, "origin", "custom-description");
}

GHashTable *
rpmostree_origin_get_packages (RpmOstreeOrigin *origin)
{
  return origin->cached_packages;
}

GHashTable *
rpmostree_origin_get_local_packages (RpmOstreeOrigin *origin)
{
  return origin->cached_local_packages;
}

GHashTable *
rpmostree_origin_get_overrides_remove (RpmOstreeOrigin *origin)
{
  return origin->cached_overrides_remove;
}

GHashTable *
rpmostree_origin_get_overrides_local_replace (RpmOstreeOrigin *origin)
{
  return origin->cached_overrides_local_replace;
}

const char *
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin)
{
  return origin->cached_override_commit;
}

gboolean
rpmostree_origin_get_regenerate_initramfs (RpmOstreeOrigin *origin)
{
  return g_key_file_get_boolean (origin->kf, "rpmostree", "regenerate-initramfs", NULL);
}

const char *const*
rpmostree_origin_get_initramfs_args (RpmOstreeOrigin *origin)
{
  return (const char * const*)origin->cached_initramfs_args;
}

const char*
rpmostree_origin_get_unconfigured_state (RpmOstreeOrigin *origin)
{
  return origin->cached_unconfigured_state;
}

/* Determines whether the origin hints at local assembly being required. In some
 * cases, no assembly might actually be required (e.g. if requested packages are
 * already in the base). IOW:
 *    FALSE --> definitely does not require local assembly
 *    TRUE  --> maybe requires assembly, need to investigate further by doing work
 */
gboolean
rpmostree_origin_may_require_local_assembly (RpmOstreeOrigin *origin)
{
  return rpmostree_origin_get_regenerate_initramfs (origin) ||
        (g_hash_table_size (origin->cached_packages) > 0) ||
        (g_hash_table_size (origin->cached_local_packages) > 0) ||
        (g_hash_table_size (origin->cached_overrides_local_replace) > 0) ||
        (g_hash_table_size (origin->cached_overrides_remove) > 0);
}

void
rpmostree_origin_get_live_state (RpmOstreeOrigin *origin,
                                 char           **out_inprogress,
                                 char           **out_live)
{
  if (out_inprogress)
    *out_inprogress = g_key_file_get_string (origin->kf, "rpmostree-ex-live", "inprogress", NULL);
  if (out_live)
    *out_live = g_key_file_get_string (origin->kf, "rpmostree-ex-live", "commit", NULL);
}

GKeyFile *
rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin)
{
  return keyfile_dup (origin->kf);
}

char *
rpmostree_origin_get_string (RpmOstreeOrigin *origin,
                             const char *section,
                             const char *value)
{
  return g_key_file_get_string (origin->kf, section, value, NULL);
}

RpmOstreeOrigin*
rpmostree_origin_ref (RpmOstreeOrigin *origin)
{
  g_assert (origin);
  origin->refcount++;
  return origin;
}

void
rpmostree_origin_unref (RpmOstreeOrigin *origin)
{
  g_assert (origin);
  g_assert_cmpint (origin->refcount, >, 0);
  origin->refcount--;
  if (origin->refcount > 0)
    return;
  g_key_file_unref (origin->kf);
  g_free (origin->cached_refspec);
  g_free (origin->cached_rojig_version);
  g_free (origin->cached_unconfigured_state);
  g_strfreev (origin->cached_initramfs_args);
  g_clear_pointer (&origin->cached_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_local_packages, g_hash_table_unref);
  g_clear_pointer (&origin->cached_overrides_local_replace, g_hash_table_unref);
  g_clear_pointer (&origin->cached_overrides_remove, g_hash_table_unref);
  g_free (origin);
}

void
rpmostree_origin_set_regenerate_initramfs (RpmOstreeOrigin *origin,
                                           gboolean regenerate,
                                           char **args)
{
  const char *section = "rpmostree";
  const char *regeneratek = "regenerate-initramfs";
  const char *argsk = "initramfs-args";

  g_strfreev (origin->cached_initramfs_args);

  if (regenerate)
    {
      g_key_file_set_boolean (origin->kf, section, regeneratek, TRUE);
      if (args && *args)
        {
          g_key_file_set_string_list (origin->kf, section, argsk,
                                      (const char *const*)args,
                                      g_strv_length (args));
        }
      else
        g_key_file_remove_key (origin->kf, section, argsk, NULL);
    }
  else
    {
      g_key_file_remove_key (origin->kf, section, regeneratek, NULL);
      g_key_file_remove_key (origin->kf, section, argsk, NULL);
    }

  origin->cached_initramfs_args =
    g_key_file_get_string_list (origin->kf, "rpmostree", "initramfs-args", NULL, NULL);
}

void
rpmostree_origin_set_override_commit (RpmOstreeOrigin *origin,
                                      const char      *checksum,
                                      const char      *version)
{
  if (checksum != NULL)
    {
      g_key_file_set_string (origin->kf, "origin", "override-commit", checksum);

      /* Add a comment with the version, to be nice. */
      if (version != NULL)
        {
          g_autofree char *comment =
            g_strdup_printf ("Version %s [%.10s]", version, checksum);
          g_key_file_set_comment (origin->kf, "origin", "override-commit", comment, NULL);
        }
    }
  else
    {
      g_key_file_remove_key (origin->kf, "origin", "override-commit", NULL);
    }

  g_free (origin->cached_override_commit);
  origin->cached_override_commit = g_strdup (checksum);
}

void
rpmostree_origin_set_rojig_version (RpmOstreeOrigin *origin,
                                    const char      *version)
{
  if (version)
    g_key_file_set_string (origin->kf, "origin", "rojig-override-version", version);
  else
    g_key_file_remove_key (origin->kf, "origin", "rojig-override-version", NULL);
  g_free (origin->cached_rojig_version);
  origin->cached_rojig_version = g_strdup (version);
}

/* The rojigRPM is highly special; it doesn't live in the rpmdb for example, as
 * that would be fully circular. Yet, it's of critical importance to the whole
 * system; we want to render it on the client. For now, what we do is stick the
 * EVR+A in the origin. That's the only data we really care about.
 *
 * Perhaps down the line, what we really want to do is store the whole Header at
 * least somewhere hooked off the deployment (or perhaps imported itself into
 * the pkgcache)?
 */
void
rpmostree_origin_set_rojig_description (RpmOstreeOrigin *origin,
                                        DnfPackage      *package)
{
  g_assert_cmpint (origin->refspec_type, ==, RPMOSTREE_REFSPEC_TYPE_ROJIG);
  g_key_file_set_string (origin->kf, "origin", "rojig-imported-evr", dnf_package_get_evr (package));
  g_key_file_set_string (origin->kf, "origin", "rojig-imported-arch", dnf_package_get_arch (package));
}

gboolean
rpmostree_origin_set_rebase_custom (RpmOstreeOrigin *origin,
                                    const char      *new_refspec,
                                    const char      *custom_origin_url,
                                    const char      *custom_origin_description,
                                    GError         **error)
{
  /* Require non-empty strings */
  if (custom_origin_url)
    {
      g_return_val_if_fail (*custom_origin_url, FALSE);
      g_return_val_if_fail (custom_origin_description && *custom_origin_description, FALSE);
    }

   /* We don't want to carry any commit overrides or version pinning during a
   * rebase by default.
   */
  rpmostree_origin_set_override_commit (origin, NULL, NULL);
  g_key_file_remove_key (origin->kf, "origin", "rojig-override-version", NULL);

  /* See related code in rpmostree_origin_parse_keyfile() */
  const char *refspecdata;
  if (!rpmostree_refspec_classify (new_refspec, &origin->refspec_type, &refspecdata, error))
    return FALSE;
  g_free (origin->cached_refspec);
  origin->cached_refspec = g_strdup (refspecdata);
  switch (origin->refspec_type)
    {
    case RPMOSTREE_REFSPEC_TYPE_CHECKSUM:
    case RPMOSTREE_REFSPEC_TYPE_OSTREE:
      {
        g_key_file_remove_key (origin->kf, "origin", "rojig", NULL);
        g_key_file_remove_key (origin->kf, "origin", "rojig-imported-evr", NULL);
        g_key_file_remove_key (origin->kf, "origin", "rojig-imported-arch", NULL);
        const char *refspec_key =
          g_key_file_has_key (origin->kf, "origin", "baserefspec", NULL) ?
          "baserefspec" : "refspec";
        g_key_file_set_string (origin->kf, "origin", refspec_key, origin->cached_refspec);
        if (!custom_origin_url)
          {
            g_key_file_remove_key (origin->kf, "origin", "custom-url", NULL);
            g_key_file_remove_key (origin->kf, "origin", "custom-description", NULL);
          }
        else
          {
            /* Custom origins have to be checksums */
            g_assert_cmpint (origin->refspec_type, ==, RPMOSTREE_REFSPEC_TYPE_CHECKSUM);
            g_key_file_set_string (origin->kf, "origin", "custom-url", custom_origin_url);
            if (custom_origin_description)
              g_key_file_set_string (origin->kf, "origin", "custom-description", custom_origin_description);
          }
      }
      break;
    case RPMOSTREE_REFSPEC_TYPE_ROJIG:
      {
        g_assert (!custom_origin_url);
        origin->cached_refspec = g_strdup (refspecdata);
        g_key_file_remove_key (origin->kf, "origin", "refspec", NULL);
        g_key_file_remove_key (origin->kf, "origin", "baserefspec", NULL);
        g_key_file_remove_key (origin->kf, "origin", "custom-url", NULL);
        g_key_file_remove_key (origin->kf, "origin", "custom-description", NULL);
        /* Peeled */
        g_key_file_set_string (origin->kf, "origin", "rojig", origin->cached_refspec);
      }
      break;
    }

  return TRUE;
}

gboolean
rpmostree_origin_set_rebase (RpmOstreeOrigin *origin,
                             const char      *new_refspec,
                             GError         **error)
{
  return rpmostree_origin_set_rebase_custom (origin, new_refspec, NULL, NULL, error);
}

/* Like g_key_file_set_string(), but remove the key if @value is NULL */
static void
set_or_unset_str (GKeyFile *kf,
                  const char *group,
                  const char *key,
                  const char *value)
{
  if (!value)
    (void) g_key_file_remove_key (kf, group, key, NULL);
  else
    (void) g_key_file_set_string (kf, group, key, value);
}

void
rpmostree_origin_set_live_state (RpmOstreeOrigin *origin,
                                 const char      *inprogress,
                                 const char      *live)
{
  if (!inprogress && !live)
    (void) g_key_file_remove_group (origin->kf, "rpmostree-ex-live", NULL);
  else
    {
      set_or_unset_str (origin->kf, "rpmostree-ex-live", "inprogress", inprogress);
      set_or_unset_str (origin->kf, "rpmostree-ex-live", "commit", live);
    }
}

static void
update_keyfile_pkgs_from_cache (RpmOstreeOrigin *origin,
                                const char      *group,
                                const char      *key,
                                GHashTable      *pkgs,
                                gboolean         has_csum)
{
  /* we're abusing a bit the concept of cache here, though
   * it's just easier to go from cache to origin */

  if (g_hash_table_size (pkgs) == 0)
    {
      g_key_file_remove_key (origin->kf, group, key, NULL);
      return;
    }

  if (has_csum)
    {
      g_autoptr(GPtrArray) pkg_csum =
        g_ptr_array_new_with_free_func (g_free);
      GLNX_HASH_TABLE_FOREACH_KV (pkgs, const char*, k, const char*, v)
        g_ptr_array_add (pkg_csum, g_strconcat (v, ":", k, NULL));
      g_key_file_set_string_list (origin->kf, group, key,
                                  (const char *const*)pkg_csum->pdata,
                                  pkg_csum->len);
    }
  else
    {
      g_autofree char **pkgv =
        (char**)g_hash_table_get_keys_as_array (pkgs, NULL);
      g_key_file_set_string_list (origin->kf, group, key,
                                  (const char *const*)pkgv,
                                  g_strv_length (pkgv));
    }

  if (g_hash_table_size (pkgs) > 0)
    {
      switch (origin->refspec_type)
        {
        case RPMOSTREE_REFSPEC_TYPE_OSTREE:
        case RPMOSTREE_REFSPEC_TYPE_CHECKSUM:
          {
            g_key_file_set_value (origin->kf, "origin", "baserefspec",
                                  origin->cached_refspec);
            g_key_file_remove_key (origin->kf, "origin", "refspec", NULL);
            break;
          }
        case RPMOSTREE_REFSPEC_TYPE_ROJIG:
          /* Nothing to switch, since libostree already doesn't know how to
           * handle rojig.
           */
          break;
        }
    }
}

gboolean
rpmostree_origin_add_packages (RpmOstreeOrigin   *origin,
                               char             **packages,
                               gboolean           local,
                               gboolean           allow_existing,
                               gboolean          *out_changed,
                               GError           **error)
{
  gboolean changed = FALSE;

  for (char **it = packages; it && *it; it++)
    {
      gboolean requested, requested_local;
      const char *pkg = *it;
      g_autofree char *sha256 = NULL;

      if (local)
        {
          if (!rpmostree_decompose_sha256_nevra (&pkg, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", pkg);
        }

      requested = g_hash_table_contains (origin->cached_packages, pkg);
      requested_local = g_hash_table_contains (origin->cached_local_packages, pkg);

      /* The list of packages is really a list of provides, so string equality
       * is a bit weird here. Multiple provides can resolve to the same package
       * and we allow that. But still, let's make sure that silly users don't
       * request the exact string. */

      /* Also note that we check in *both* the requested and the requested-local
       * list: requested-local pkgs are treated like requested pkgs in the core.
       * The only "magical" thing about them is that requested-local pkgs are
       * specifically looked for in the pkgcache. Additionally, making sure the
       * strings are unique allow `rpm-ostree uninstall` to know exactly what
       * the user means. */

      if (requested || requested_local)
        {
          if (allow_existing)
            continue;

          if (requested)
            return glnx_throw (error, "Package/capability '%s' is already requested", pkg);
          else
            return glnx_throw (error, "Package '%s' is already layered", pkg);
        }

      if (local)
        g_hash_table_insert (origin->cached_local_packages,
                             g_strdup (pkg), g_steal_pointer (&sha256));
      else
        g_hash_table_add (origin->cached_packages, g_strdup (pkg));
      changed = TRUE;
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, "packages",
                                    local ? "requested-local" : "requested",
                                    local ? origin->cached_local_packages
                                          : origin->cached_packages, local);

  *out_changed = changed;
  return TRUE;
}

static gboolean
build_name_to_nevra_map (GHashTable  *nevras,
                         GHashTable **out_name_to_nevra,
                         GError     **error)
{
  g_autoptr(GHashTable) name_to_nevra = /* nevra vals owned by @nevras */
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GLNX_HASH_TABLE_FOREACH (nevras, const char*, nevra)
    {
      g_autofree char *name = NULL;
      if (!rpmostree_decompose_nevra (nevra, &name, NULL, NULL, NULL, NULL, error))
        return FALSE;
      g_hash_table_insert (name_to_nevra, g_steal_pointer (&name), (gpointer)nevra);
    }

  *out_name_to_nevra = g_steal_pointer (&name_to_nevra);
  return TRUE;
}

gboolean
rpmostree_origin_remove_packages (RpmOstreeOrigin  *origin,
                                  char            **packages,
                                  gboolean          allow_noent,
                                  gboolean         *out_changed,
                                  GError          **error)
{
  gboolean changed = FALSE;
  gboolean local_changed = FALSE;

  /* lazily calculated */
  g_autoptr(GHashTable) name_to_nevra = NULL;

  for (char **it = packages; it && *it; it++)
    {
      /* really, either a NEVRA (local RPM) or freeform provides request (from repo) */
      const char *package = *it;
      if (g_hash_table_remove (origin->cached_local_packages, package))
        local_changed = TRUE;
      else if (g_hash_table_remove (origin->cached_packages, package))
        changed = TRUE;
      else
        {
          if (!name_to_nevra)
            {
              if (!build_name_to_nevra_map (origin->cached_local_packages,
                                            &name_to_nevra, error))
                return FALSE;
            }

          if (g_hash_table_contains (name_to_nevra, package))
            {
              g_assert (g_hash_table_remove (origin->cached_local_packages,
                                             g_hash_table_lookup (name_to_nevra, package)));
              local_changed = TRUE;
            }
          else if (!allow_noent)
            return glnx_throw (error, "Package/capability '%s' is not currently requested",
                               package);
        }
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested",
                                    origin->cached_packages, FALSE);
  if (local_changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested-local",
                                    origin->cached_local_packages, TRUE);

  *out_changed = changed || local_changed;
  return TRUE;
}

gboolean
rpmostree_origin_remove_all_packages (RpmOstreeOrigin  *origin,
                                      gboolean         *out_changed,
                                      GError          **error)
{
  gboolean changed = FALSE;
  gboolean local_changed = FALSE;

  if (g_hash_table_size (origin->cached_packages) > 0)
    {
      g_hash_table_remove_all (origin->cached_packages);
      changed = TRUE;
    }

  if (g_hash_table_size (origin->cached_local_packages) > 0)
    {
      g_hash_table_remove_all (origin->cached_local_packages);
      local_changed = TRUE;
    }

  if (changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested",
                                    origin->cached_packages, FALSE);
  if (local_changed)
    update_keyfile_pkgs_from_cache (origin, "packages", "requested-local",
                                    origin->cached_local_packages, TRUE);
  if (out_changed)
    *out_changed = changed || local_changed;
  return TRUE;
}

gboolean
rpmostree_origin_add_overrides (RpmOstreeOrigin  *origin,
                                char            **packages,
                                RpmOstreeOriginOverrideType type,
                                GError          **error)
{
  gboolean changed = FALSE;
  for (char **it = packages; it && *it; it++)
    {
      const char *pkg = *it;
      g_autofree char *sha256 = NULL;

      if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
        {
          if (!rpmostree_decompose_sha256_nevra (&pkg, &sha256, error))
            return glnx_throw (error, "Invalid SHA-256 NEVRA string: %s", pkg);
        }

      /* Check that the same overrides don't already exist. Of course, in the local replace
       * case, this doesn't catch same pkg name but different EVRA; we'll just barf at that
       * later on in the core. This is just an early easy sanity check. */
      if (g_hash_table_contains (origin->cached_overrides_remove, pkg) ||
          g_hash_table_contains (origin->cached_overrides_local_replace, pkg))
        return glnx_throw (error, "Override already exists for package '%s'", pkg);

      if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
        g_hash_table_insert (origin->cached_overrides_local_replace, g_strdup (pkg),
                             g_steal_pointer (&sha256));
      else if (type == RPMOSTREE_ORIGIN_OVERRIDE_REMOVE)
        g_hash_table_add (origin->cached_overrides_remove, g_strdup (pkg));
      else
        g_assert_not_reached ();

      changed = TRUE;
    }

  if (changed)
    {
      if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
        update_keyfile_pkgs_from_cache (origin, "overrides", "replace-local",
                                        origin->cached_overrides_local_replace, TRUE);
      else if (type == RPMOSTREE_ORIGIN_OVERRIDE_REMOVE)
        update_keyfile_pkgs_from_cache (origin, "overrides", "remove",
                                        origin->cached_overrides_remove, FALSE);
      else
        g_assert_not_reached ();
    }

  return TRUE;
}

/* Returns FALSE if the override does not exist. */
gboolean
rpmostree_origin_remove_override (RpmOstreeOrigin  *origin,
                                  const char       *package,
                                  RpmOstreeOriginOverrideType type)
{
  if (type == RPMOSTREE_ORIGIN_OVERRIDE_REPLACE_LOCAL)
    {
      if (!g_hash_table_remove (origin->cached_overrides_local_replace, package))
        return FALSE;
      update_keyfile_pkgs_from_cache (origin, "overrides", "replace-local",
                                      origin->cached_overrides_local_replace, TRUE);
    }
  else if (type == RPMOSTREE_ORIGIN_OVERRIDE_REMOVE)
    {
      if (!g_hash_table_remove (origin->cached_overrides_remove, package))
        return FALSE;
      update_keyfile_pkgs_from_cache (origin, "overrides", "remove",
                                      origin->cached_overrides_remove, FALSE);
    }
  else
    g_assert_not_reached ();

  return TRUE;
}

gboolean
rpmostree_origin_remove_all_overrides (RpmOstreeOrigin  *origin,
                                       gboolean         *out_changed,
                                       GError          **error)
{
  gboolean remove_changed = (g_hash_table_size (origin->cached_overrides_remove) > 0);
  g_hash_table_remove_all (origin->cached_overrides_remove);

  gboolean local_replace_changed =
    (g_hash_table_size (origin->cached_overrides_local_replace) > 0);
  g_hash_table_remove_all (origin->cached_overrides_local_replace);

  if (remove_changed)
    update_keyfile_pkgs_from_cache (origin, "overrides", "remove",
                                    origin->cached_overrides_remove, FALSE);
  if (local_replace_changed)
    update_keyfile_pkgs_from_cache (origin, "overrides", "replace-local",
                                    origin->cached_overrides_local_replace, TRUE);
  if (out_changed)
    *out_changed = remove_changed || local_replace_changed;
  return TRUE;
}
