/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <libglnx.h>
#include <systemd/sd-journal.h>
#include "rpmostreed-utils.h"
#include "rpmostree-util.h"

#include "rpmostree-sysroot-upgrader.h"
#include "rpmostree-core.h"
#include "rpmostree-origin.h"
#include "rpmostree-kernel.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-output.h"
#include "rpmostree-scripts.h"

#include "ostree-repo.h"

/**
 * SECTION:rpmostree-sysroot-upgrader
 * @title: Simple upgrade class
 * @short_description: Upgrade RPM+OSTree systems
 *
 * The #RpmOstreeSysrootUpgrader class models a `baserefspec` OSTree branch
 * in an origin file, along with a set of layered RPM packages.
 *
 * It also supports the plain-ostree "refspec" model.
 */
typedef struct {
  GObjectClass parent_class;
} RpmOstreeSysrootUpgraderClass;

struct RpmOstreeSysrootUpgrader {
  GObject parent;

  OstreeSysroot *sysroot;
  OstreeRepo *repo;
  char *osname;
  RpmOstreeSysrootUpgraderFlags flags;

  OstreeDeployment *cfg_merge_deployment;
  OstreeDeployment *origin_merge_deployment;
  RpmOstreeOrigin *origin;

  /* Used during tree construction */
  OstreeRepoDevInoCache *devino_cache;
  int tmprootfs_dfd;
  RpmOstreeRefSack *rsack;
  GLnxTmpDir metatmpdir;
  RpmOstreeContext *ctx;

  GPtrArray *overlay_packages; /* Finalized list of pkgs to overlay */
  GPtrArray *override_remove_packages; /* Finalized list of base pkgs to remove */
  GPtrArray *override_replace_local_packages; /* Finalized list of local base pkgs to replace */

  gboolean layering_initialized; /* Whether layering_type is known */
  RpmOstreeSysrootUpgraderLayeringType layering_type;
  gboolean layering_changed; /* Whether changes to layering should result in a new commit */
  gboolean pkgs_imported; /* Whether pkgs to be layered have been downloaded & imported */
  char *base_revision; /* Non-layered replicated commit */
  char *final_revision; /* Computed by layering; if NULL, only using base_revision */

  char **kargs_strv; /* Kernel argument list to be written into deployment  */
};

enum {
  PROP_0,

  PROP_SYSROOT,
  PROP_OSNAME,
  PROP_FLAGS
};

static void rpmostree_sysroot_upgrader_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (RpmOstreeSysrootUpgrader, rpmostree_sysroot_upgrader, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, rpmostree_sysroot_upgrader_initable_iface_init))

static gboolean
parse_origin_deployment (RpmOstreeSysrootUpgrader *self,
                         OstreeDeployment         *deployment,
                         GCancellable           *cancellable,
                         GError                **error)
{
  self->origin = rpmostree_origin_parse_deployment (deployment, error);
  if (!self->origin)
    return FALSE;

  if (rpmostree_origin_get_unconfigured_state (self->origin) &&
      !(self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED))
    {
      /* explicit action is required OS creator to upgrade, print their text as an error */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "origin unconfigured-state: %s",
                   rpmostree_origin_get_unconfigured_state (self->origin));
      return FALSE;
    }

  /* A bit hacky; here we clean out the live state which is deployment specific.
   * We don't expect users of the upgrader to want the live state.
   */
  rpmostree_origin_set_live_state (self->origin, NULL, NULL);

  return TRUE;
}

static gboolean
rpmostree_sysroot_upgrader_initable_init (GInitable        *initable,
                                          GCancellable     *cancellable,
                                          GError          **error)
{
  RpmOstreeSysrootUpgrader *self = (RpmOstreeSysrootUpgrader*)initable;

  OstreeDeployment *booted_deployment =
    ostree_sysroot_get_booted_deployment (self->sysroot);
  if (booted_deployment == NULL && self->osname == NULL)
    return glnx_throw (error, "Not currently booted into an OSTree system and no OS specified");

  if (self->osname == NULL)
    {
      g_assert (booted_deployment);
      self->osname = g_strdup (ostree_deployment_get_osname (booted_deployment));
    }
  else if (self->osname[0] == '\0')
    return glnx_throw (error, "Invalid empty osname");

  if (!ostree_sysroot_get_repo (self->sysroot, &self->repo, cancellable, error))
    return FALSE;

  self->cfg_merge_deployment =
    ostree_sysroot_get_merge_deployment (self->sysroot, self->osname);
  self->origin_merge_deployment =
    rpmostree_syscore_get_origin_merge_deployment (self->sysroot, self->osname);
  if (self->cfg_merge_deployment == NULL || self->origin_merge_deployment == NULL)
    return glnx_throw (error, "No previous deployment for OS '%s'",
                       self->osname);

  /* Should we consider requiring --discard-hotfix here?
   * See also `ostree admin upgrade` bits.
   */
  if (!parse_origin_deployment (self, self->origin_merge_deployment,
                                cancellable, error))
    return FALSE;

  const char *merge_deployment_csum =
    ostree_deployment_get_csum (self->origin_merge_deployment);

  /* Now, load starting base/final checksums.  We may end up changing
   * one or both if we upgrade.  But we also want to support redeploying
   * without changing them.
   */
  gboolean is_layered;
  if (!rpmostree_deployment_get_layered_info (self->repo,
                                              self->origin_merge_deployment,
                                              &is_layered, &self->base_revision,
                                              NULL, NULL, NULL, error))
    return FALSE;

  if (is_layered)
    /* base layer is already populated above */
    self->final_revision = g_strdup (merge_deployment_csum);
  else
    self->base_revision = g_strdup (merge_deployment_csum);

  g_assert (self->base_revision);
  return TRUE;
}

static void
rpmostree_sysroot_upgrader_initable_iface_init (GInitableIface *iface)
{
  iface->init = rpmostree_sysroot_upgrader_initable_init;
}

static void
rpmostree_sysroot_upgrader_finalize (GObject *object)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  g_clear_pointer (&self->rsack, rpmostree_refsack_unref);
  g_clear_object (&self->ctx);
  glnx_close_fd (&self->tmprootfs_dfd);

  (void)glnx_tmpdir_delete (&self->metatmpdir, NULL, NULL);

  g_clear_pointer (&self->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->repo);
  g_free (self->osname);

  g_clear_object (&self->cfg_merge_deployment);
  g_clear_object (&self->origin_merge_deployment);
  g_clear_pointer (&self->origin, (GDestroyNotify)rpmostree_origin_unref);
  g_free (self->base_revision);
  g_free (self->final_revision);
  g_strfreev (self->kargs_strv);
  g_clear_pointer (&self->overlay_packages, (GDestroyNotify)g_ptr_array_unref);
  g_clear_pointer (&self->override_remove_packages, (GDestroyNotify)g_ptr_array_unref);

  G_OBJECT_CLASS (rpmostree_sysroot_upgrader_parent_class)->finalize (object);
}

static void
rpmostree_sysroot_upgrader_set_property (GObject         *object,
                                         guint            prop_id,
                                         const GValue    *value,
                                         GParamSpec      *pspec)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  switch (prop_id)
    {
    case PROP_SYSROOT:
      self->sysroot = g_value_dup_object (value);
      break;
    case PROP_OSNAME:
      self->osname = g_value_dup_string (value);
      break;
    case PROP_FLAGS:
      self->flags = g_value_get_flags (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
rpmostree_sysroot_upgrader_get_property (GObject         *object,
                                         guint            prop_id,
                                         GValue          *value,
                                         GParamSpec      *pspec)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  switch (prop_id)
    {
    case PROP_SYSROOT:
      g_value_set_object (value, self->sysroot);
      break;
    case PROP_OSNAME:
      g_value_set_string (value, self->osname);
      break;
    case PROP_FLAGS:
      g_value_set_flags (value, self->flags);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
rpmostree_sysroot_upgrader_constructed (GObject *object)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  g_assert (self->sysroot != NULL);

  G_OBJECT_CLASS (rpmostree_sysroot_upgrader_parent_class)->constructed (object);
}

static void
rpmostree_sysroot_upgrader_class_init (RpmOstreeSysrootUpgraderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = rpmostree_sysroot_upgrader_constructed;
  object_class->get_property = rpmostree_sysroot_upgrader_get_property;
  object_class->set_property = rpmostree_sysroot_upgrader_set_property;
  object_class->finalize = rpmostree_sysroot_upgrader_finalize;

  g_object_class_install_property (object_class,
                                   PROP_SYSROOT,
                                   g_param_spec_object ("sysroot", "", "",
                                                        OSTREE_TYPE_SYSROOT,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class,
                                   PROP_OSNAME,
                                   g_param_spec_string ("osname", "", "", NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class,
                                   PROP_FLAGS,
                                   g_param_spec_flags ("flags", "", "",
                                                       rpmostree_sysroot_upgrader_flags_get_type (),
                                                       0,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
rpmostree_sysroot_upgrader_init (RpmOstreeSysrootUpgrader *self)
{
  self->tmprootfs_dfd = -1;
}

/**
 * rpmostree_sysroot_upgrader_new_for_os_with_flags:
 * @sysroot: An #OstreeSysroot
 * @osname: (allow-none): Operating system name
 * @flags: Flags
 *
 * Returns: (transfer full): An upgrader
 */
RpmOstreeSysrootUpgrader *
rpmostree_sysroot_upgrader_new (OstreeSysroot              *sysroot,
                                const char                 *osname,
                                RpmOstreeSysrootUpgraderFlags  flags,
                                GCancellable               *cancellable,
                                GError                    **error)
{
  return g_initable_new (RPMOSTREE_TYPE_SYSROOT_UPGRADER, cancellable, error,
                         "sysroot", sysroot, "osname", osname, "flags", flags, NULL);
}

RpmOstreeOrigin *
rpmostree_sysroot_upgrader_dup_origin (RpmOstreeSysrootUpgrader *self)
{
  return rpmostree_origin_dup (self->origin);
}

void
rpmostree_sysroot_upgrader_set_origin (RpmOstreeSysrootUpgrader *self,
                                       RpmOstreeOrigin          *new_origin)
{
  rpmostree_origin_unref (self->origin);
  self->origin = rpmostree_origin_dup (new_origin);
}

const char *
rpmostree_sysroot_upgrader_get_base (RpmOstreeSysrootUpgrader *self)
{
  return self->base_revision;
}

OstreeDeployment*
rpmostree_sysroot_upgrader_get_merge_deployment (RpmOstreeSysrootUpgrader *self)
{
  return self->origin_merge_deployment;
}

/*
 * Like ostree_sysroot_upgrader_pull(), but also handles the `baserefspec` we
 * use when doing layered packages.
 */
gboolean
rpmostree_sysroot_upgrader_pull_base (RpmOstreeSysrootUpgrader  *self,
                                      const char             *dir_to_pull,
                                      OstreeRepoPullFlags     flags,
                                      OstreeAsyncProgress    *progress,
                                      gboolean               *out_changed,
                                      GCancellable           *cancellable,
                                      GError                **error)
{
  const char *refspec = rpmostree_origin_get_refspec (self->origin);

  g_autofree char *origin_remote = NULL;
  g_autofree char *origin_ref = NULL;
  if (!ostree_parse_refspec (refspec, &origin_remote, &origin_ref, error))
    return FALSE;

  const gboolean allow_older =
    (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER) > 0;
  const gboolean synthetic =
    (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_SYNTHETIC_PULL) > 0;

  const char *override_commit = rpmostree_origin_get_override_commit (self->origin);

  g_assert (self->origin_merge_deployment);
  if (origin_remote && !synthetic)
    {
      g_autoptr(GVariantBuilder) optbuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      if (dir_to_pull && *dir_to_pull)
        g_variant_builder_add (optbuilder, "{s@v}", "subdir",
                               g_variant_new_variant (g_variant_new_string (dir_to_pull)));
      g_variant_builder_add (optbuilder, "{s@v}", "flags",
                             g_variant_new_variant (g_variant_new_int32 (flags)));
      /* Add the timestamp check, unless disabled. The option was added in
       * libostree v2017.11 */
      if (!allow_older)
        g_variant_builder_add (optbuilder, "{s@v}", "timestamp-check",
                               g_variant_new_variant (g_variant_new_boolean (TRUE)));
      g_variant_builder_add (optbuilder, "{s@v}", "refs",
                             g_variant_new_variant (g_variant_new_strv (
                                 (const char *const *)&origin_ref, 1)));
      if (override_commit)
        g_variant_builder_add (optbuilder, "{s@v}", "override-commit-ids",
                               g_variant_new_variant (g_variant_new_strv (
                                   (const char *const *)&override_commit, 1)));

      g_autoptr(GVariant) opts = g_variant_ref_sink (g_variant_builder_end (optbuilder));
      if (!ostree_repo_pull_with_options (self->repo, origin_remote, opts, progress,
                                          cancellable, error))
        return FALSE;

      if (progress)
        ostree_async_progress_finish (progress);
    }

  g_autofree char *new_base_rev = NULL;
  if (override_commit)
    new_base_rev = g_strdup (override_commit);
  else
    {
      if (!ostree_repo_resolve_rev (self->repo, refspec, FALSE, &new_base_rev, error))
        return FALSE;
    }

  gboolean changed = !g_str_equal (new_base_rev, self->base_revision);
  if (changed)
    {
      /* check timestamps here too in case the commit was already pulled, or the pull was
       * synthetic, or the refspec is local */
      if (!allow_older)
        {
          if (!ostree_sysroot_upgrader_check_timestamps (self->repo, self->base_revision,
                                                         new_base_rev, error))
            return FALSE;
        }

      g_free (self->base_revision);
      self->base_revision = g_steal_pointer (&new_base_rev);
    }

  *out_changed = changed;
  return TRUE;
}

static gboolean
checkout_base_tree (RpmOstreeSysrootUpgrader *self,
                    GCancellable          *cancellable,
                    GError               **error)
{
  g_assert_cmpint (self->tmprootfs_dfd, ==, -1);

  /* let's give the user some feedback so they don't think we're blocked */
  rpmostree_output_task_begin ("Checking out tree %.7s", self->base_revision);

  int repo_dfd = ostree_repo_get_dfd (self->repo); /* borrowed */
  /* Always delete this */
  if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_OLD_TMP_ROOTFS_DIR,
                             cancellable, error))
    return FALSE;

  /* Make the parents with default mode */
  if (!glnx_shutil_mkdir_p_at (repo_dfd,
                               dirname (strdupa (RPMOSTREE_TMP_PRIVATE_DIR)),
                               0755, cancellable, error))
    return FALSE;

  /* And this dir should always be 0700, to ensure that when we checkout
   * world-writable dirs like /tmp it's not accessible to unprivileged users.
   */
  if (!glnx_shutil_mkdir_p_at (repo_dfd,
                               RPMOSTREE_TMP_PRIVATE_DIR,
                               0700, cancellable, error))
    return FALSE;

  /* delete dir in case a previous run didn't finish successfully */
  if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR,
                             cancellable, error))
    return FALSE;

  /* NB: we let ostree create the dir for us so that the root dir has the
   * correct xattrs (e.g. selinux label) */
  self->devino_cache = ostree_repo_devino_cache_new ();
  OstreeRepoCheckoutAtOptions checkout_options =
    { .devino_to_csum_cache = self->devino_cache };
  if (!ostree_repo_checkout_at (self->repo, &checkout_options,
                                repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR,
                                self->base_revision, cancellable, error))
    return FALSE;

  if (!glnx_opendirat (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR, FALSE,
                       &self->tmprootfs_dfd, error))
    return FALSE;

  /* build a centralized rsack for it, since we need it in a few places */
  self->rsack = rpmostree_get_refsack_for_root (self->tmprootfs_dfd, ".", error);
  if (self->rsack == NULL)
    return FALSE;

  rpmostree_output_task_end ("done");

  return TRUE;
}

/* XXX: This is ugly, but the alternative is to de-couple RpmOstreeTreespec from
 * RpmOstreeContext, which also use it for hashing and store it directly in
 * assembled commit metadata. Probably assemble_commit() should live somewhere
 * else, maybe directly in `container-builtins.c`. */
static RpmOstreeTreespec *
generate_treespec (RpmOstreeSysrootUpgrader *self)
{
  g_autoptr(GKeyFile) treespec = g_key_file_new ();

  if (self->overlay_packages->len > 0)
    {
      g_key_file_set_string_list (treespec, "tree", "packages",
                                  (const char* const*)self->overlay_packages->pdata,
                                  self->overlay_packages->len);
    }

  GHashTable *local_packages = rpmostree_origin_get_local_packages (self->origin);
  if (g_hash_table_size (local_packages) > 0)
    {
      g_autoptr(GPtrArray) sha256_nevra = g_ptr_array_new_with_free_func (g_free);

      GLNX_HASH_TABLE_FOREACH_KV (local_packages, const char*, k, const char*, v)
        g_ptr_array_add (sha256_nevra, g_strconcat (v, ":", k, NULL));

      g_key_file_set_string_list (treespec, "tree", "cached-packages",
                                  (const char* const*)sha256_nevra->pdata,
                                  sha256_nevra->len);
    }

  if (self->override_replace_local_packages->len > 0)
    {
      g_key_file_set_string_list (treespec, "tree", "cached-replaced-base-packages",
                                  (const char* const*)self->override_replace_local_packages->pdata,
                                  self->override_replace_local_packages->len);
    }

  if (self->override_remove_packages->len > 0)
    {
      g_key_file_set_string_list (treespec, "tree", "removed-base-packages",
                                  (const char* const*)self->override_remove_packages->pdata,
                                  self->override_remove_packages->len);
    }

  return rpmostree_treespec_new_from_keyfile (treespec, NULL);
}

static gboolean
initialize_metatmpdir (RpmOstreeSysrootUpgrader *self,
                       GError                  **error)
{
  if (self->metatmpdir.initialized)
    return TRUE; /* already initialized; return early */

  if (!glnx_mkdtemp ("rpmostree-localpkgmeta-XXXXXX", 0700,
                     &self->metatmpdir, error))
    return FALSE;

  return TRUE;
}

static gboolean
finalize_removal_overrides (RpmOstreeSysrootUpgrader *self,
                            GCancellable             *cancellable,
                            GError                  **error)
{
  GHashTable *removals = rpmostree_origin_get_overrides_remove (self->origin);
  g_autoptr(GPtrArray) ret_final_removals = g_ptr_array_new_with_free_func (g_free);

  g_autoptr(GPtrArray) inactive_removals = g_ptr_array_new ();
  GLNX_HASH_TABLE_FOREACH (removals, const char*, pkgname)
    {
      g_autoptr(DnfPackage) pkg = NULL;
      if (!rpmostree_sack_get_by_pkgname (self->rsack->sack, pkgname, &pkg, error))
        return FALSE;

      if (pkg)
        g_ptr_array_add (ret_final_removals, g_strdup (pkgname));
      else
        g_ptr_array_add (inactive_removals, (gpointer)pkgname);
    }

  if (inactive_removals->len > 0)
    {
      rpmostree_output_message ("Inactive base removals:");
      for (guint i = 0; i < inactive_removals->len; i++)
        rpmostree_output_message ("  %s", (const char*)inactive_removals->pdata[i]);
    }

  g_assert (!self->override_remove_packages);
  self->override_remove_packages = g_steal_pointer (&ret_final_removals);
  return TRUE;
}

static gboolean
finalize_replacement_overrides (RpmOstreeSysrootUpgrader *self,
                                GCancellable             *cancellable,
                                GError                  **error)
{
  GHashTable *local_replacements =
    rpmostree_origin_get_overrides_local_replace (self->origin);
  g_autoptr(GPtrArray) ret_final_local_replacements =
    g_ptr_array_new_with_free_func (g_free);

  g_autoptr(GPtrArray) inactive_replacements = g_ptr_array_new ();

  GLNX_HASH_TABLE_FOREACH_KV (local_replacements, const char*, nevra, const char*, sha256)
    {
      /* use the pkgcache because there's no safe way to go from nevra --> pkgname */
      g_autofree char *pkgname = NULL;
      if (!rpmostree_get_nevra_from_pkgcache (self->repo, nevra, &pkgname, NULL, NULL,
                                              NULL, NULL, cancellable, error))
        return FALSE;

      g_autoptr(DnfPackage) pkg = NULL;
      if (!rpmostree_sack_get_by_pkgname (self->rsack->sack, pkgname, &pkg, error))
        return FALSE;

      /* make inactive if it's missing or if that exact nevra is already present */
      if (pkg && !rpmostree_sack_has_subject (self->rsack->sack, nevra))
        {
          g_ptr_array_add (ret_final_local_replacements,
                           g_strconcat (sha256, ":", nevra, NULL));
        }
      else
        g_ptr_array_add (inactive_replacements, (gpointer)nevra);
    }

  if (inactive_replacements->len > 0)
    {
      rpmostree_output_message ("Inactive base replacements:");
      for (guint i = 0; i < inactive_replacements->len; i++)
        rpmostree_output_message ("  %s", (const char*)inactive_replacements->pdata[i]);
    }

  g_assert (!self->override_replace_local_packages);
  self->override_replace_local_packages = g_steal_pointer (&ret_final_local_replacements);
  return TRUE;
}

static gboolean
finalize_overrides (RpmOstreeSysrootUpgrader *self,
                    GCancellable             *cancellable,
                    GError                  **error)
{
  return finalize_removal_overrides (self, cancellable, error)
      && finalize_replacement_overrides (self, cancellable, error);
}

/* Go through rpmdb and jot down the missing pkgs from the given set. Really, we
 * don't *have* to do this: we could just give everything to libdnf and let it
 * figure out what is already installed. The advantage of doing it ourselves is
 * that we can optimize for the case where no packages are missing and we don't
 * have to fetch metadata. Another advantage is that we can make sure that the
 * embedded treespec only contains the provides we *actually* layer, which is
 * needed for displaying to the user (though we could fetch that info from
 * libdnf after resolution).
 * */
static gboolean
finalize_overlays (RpmOstreeSysrootUpgrader *self,
                   GCancellable             *cancellable,
                   GError                  **error)
{
  /* request (owned by origin) --> providing nevra */
  g_autoptr(GHashTable) inactive_requests =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  g_autoptr(GPtrArray) ret_missing_pkgs = g_ptr_array_new_with_free_func (g_free);

  /* Add the local pkgs as if they were installed: since they're unconditionally
   * layered, we treat them as part of the base wrt regular requested pkgs. E.g.
   * you can have foo-1.0-1.x86_64 layered, and foo or /usr/bin/foo as dormant.
   * */
  GHashTable *local_pkgs = rpmostree_origin_get_local_packages (self->origin);
  if (g_hash_table_size (local_pkgs) > 0)
    {
      if (!initialize_metatmpdir (self, error))
        return FALSE;

      GLNX_HASH_TABLE_FOREACH_KV (local_pkgs, const char*, nevra, const char*, sha256)
        {
          g_autoptr(GVariant) header = NULL;
          g_autofree char *path =
            g_strdup_printf ("%s/%s.rpm", self->metatmpdir.path, nevra);

          if (!rpmostree_pkgcache_find_pkg_header (self->repo, nevra, sha256,
                                                   &header, cancellable, error))
            return FALSE;

          if (!glnx_file_replace_contents_at (AT_FDCWD, path,
                                              g_variant_get_data (header),
                                              g_variant_get_size (header),
                                              GLNX_FILE_REPLACE_NODATASYNC,
                                              cancellable, error))
            return FALSE;

          /* Also check if that exact NEVRA is already in the root (if the pkg
           * exists, but is a different EVR, depsolve will catch that). In the
           * future, we'll allow packages to replace base pkgs. */
          if (rpmostree_sack_has_subject (self->rsack->sack, nevra))
            return glnx_throw (error, "Package '%s' is already in the base", nevra);

          dnf_sack_add_cmdline_package (self->rsack->sack, path);
        }
    }

  GHashTable *removals = rpmostree_origin_get_overrides_remove (self->origin);

  /* check for each package if we have a provides or a path match */
  GLNX_HASH_TABLE_FOREACH (rpmostree_origin_get_packages (self->origin),
                           const char*, pattern)
    {
      g_autoptr(GPtrArray) matches =
        rpmostree_get_matching_packages (self->rsack->sack, pattern);

      if (matches->len == 0)
        {
          /* no matches, so we'll need to layer it */
          g_ptr_array_add (ret_missing_pkgs, g_strdup (pattern));
          continue;
        }

      /* Error out if it matches a base package that was also requested to be removed.
       * Conceptually, we want users to use override replacements, not remove+overlay.
       * Really, we could just not do this check; it'd just end up being dormant, though
       * that might be confusing to users. */
      for (guint i = 0; i < matches->len; i++)
        {
          DnfPackage *pkg = matches->pdata[i];
          const char *name = dnf_package_get_name (pkg);
          const char *repo = dnf_package_get_reponame (pkg);

          if (g_strcmp0 (repo, HY_CMDLINE_REPO_NAME) == 0)
            continue; /* local RPM added up above */

          if (g_hash_table_contains (removals, name))
            return glnx_throw (error,
                               "Cannot request '%s' provided by removed package '%s'",
                               pattern, dnf_package_get_nevra (pkg));
        }

      /* Otherwise, it's an inactive request: remember them so we can print a nice notice.
       * Just use the first package as the "providing" pkg. */
      const char *providing_nevra = dnf_package_get_nevra (matches->pdata[0]);
      g_hash_table_insert (inactive_requests, (gpointer)pattern,
                           g_strdup (providing_nevra));
    }

  if (g_hash_table_size (inactive_requests) > 0)
    {
      rpmostree_output_message ("Inactive requests:");
      GLNX_HASH_TABLE_FOREACH_KV (inactive_requests, const char*, req, const char*, nevra)
        rpmostree_output_message ("  %s (already provided by %s)", req, nevra);
    }

  g_assert (!self->overlay_packages);
  self->overlay_packages = g_steal_pointer (&ret_missing_pkgs);
  return TRUE;
}

static gboolean
prepare_context_for_assembly (RpmOstreeSysrootUpgrader *self,
                              const char               *tmprootfs,
                              GCancellable             *cancellable,
                              GError                  **error)
{
  /* make sure yum repos and passwd used are from our cfg merge */
  rpmostree_context_configure_from_deployment (self->ctx, self->sysroot,
                                               self->cfg_merge_deployment);

  /* load the sepolicy to use during import */
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;
  if (!rpmostree_prepare_rootfs_get_sepolicy (self->tmprootfs_dfd, &sepolicy,
                                              cancellable, error))
    return FALSE;

  rpmostree_context_set_sepolicy (self->ctx, sepolicy);

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGCACHE_ONLY)
    rpmostree_context_set_pkgcache_only (self->ctx, TRUE);

  return TRUE;
}

/* Initialize libdnf context from our configuration */
static gboolean
prep_local_assembly (RpmOstreeSysrootUpgrader *self,
                     GCancellable             *cancellable,
                     GError                  **error)
{
  g_assert (!self->ctx);
  self->ctx = rpmostree_context_new_system (self->repo, cancellable, error);
  g_autofree char *tmprootfs_abspath = glnx_fdrel_abspath (self->tmprootfs_dfd, ".");

  if (!prepare_context_for_assembly (self, tmprootfs_abspath, cancellable, error))
    return FALSE;

  GHashTable *local_pkgs = rpmostree_origin_get_local_packages (self->origin);

  /* NB: We're pretty much using the defaults for the other treespec values like
   * instlang and docs since it would be hard to expose the cli for them because
   * they wouldn't affect just the new pkgs, but even previously added ones. */
  g_autoptr(RpmOstreeTreespec) treespec = generate_treespec (self);
  if (treespec == NULL)
    return FALSE;

  if (!rpmostree_context_setup (self->ctx, tmprootfs_abspath, tmprootfs_abspath, treespec,
                                cancellable, error))
    return FALSE;

  const gboolean have_packages = (self->overlay_packages->len > 0 ||
                                  g_hash_table_size (local_pkgs) > 0 ||
                                  self->override_remove_packages->len > 0 ||
                                  self->override_replace_local_packages->len > 0);
  if (have_packages)
    {
      if (!rpmostree_context_prepare (self->ctx, cancellable, error))
        return FALSE;
      self->layering_type = RPMOSTREE_SYSROOT_UPGRADER_LAYERING_RPMMD_REPOS;
    }
  else
    {
      rpmostree_context_set_is_empty (self->ctx);
      self->layering_type = RPMOSTREE_SYSROOT_UPGRADER_LAYERING_LOCAL;
    }

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN)
    {
      if (have_packages)
        rpmostree_print_transaction (rpmostree_context_get_dnf (self->ctx));
    }

  /* If the current state has layering, compare the depsolved set for changes. */
  if (self->final_revision)
    {
      g_autoptr(GVariant) prev_commit = NULL;

      if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     self->final_revision, &prev_commit, error))
        return FALSE;

      g_autoptr(GVariant) metadata = g_variant_get_child_value (prev_commit, 0);
      g_autoptr(GVariantDict) metadata_dict = g_variant_dict_new (metadata);
      g_autoptr(GVariant) previous_sha512_v =
        _rpmostree_vardict_lookup_value_required (metadata_dict, "rpmostree.state-sha512",
                                                  (GVariantType*)"s", error);
      if (!previous_sha512_v)
        return FALSE;
      const char *previous_state_sha512 = g_variant_get_string (previous_sha512_v, NULL);
      g_autofree char *new_state_sha512 = NULL;
      if (!rpmostree_context_get_state_sha512 (self->ctx, &new_state_sha512, error))
        return FALSE;

      self->layering_changed = strcmp (previous_state_sha512, new_state_sha512) != 0;
    }
  else
    /* Otherwise, we're transitioning from not-layered to layered, so it
       definitely changed */
    self->layering_changed = TRUE;

  return TRUE;
}

/* Overlay pkgs, run scripts, and commit final rootfs to ostree */
static gboolean
perform_local_assembly (RpmOstreeSysrootUpgrader *self,
                        GCancellable             *cancellable,
                        GError                  **error)
{
  g_assert (self->layering_initialized);
  g_assert (self->pkgs_imported);

  /* If we computed no layering is required, we're done */
  if (self->layering_type == RPMOSTREE_SYSROOT_UPGRADER_LAYERING_NONE)
    return TRUE;

  rpmostree_context_set_devino_cache (self->ctx, self->devino_cache);
  rpmostree_context_set_tmprootfs_dfd (self->ctx, self->tmprootfs_dfd);

  if (self->layering_type == RPMOSTREE_SYSROOT_UPGRADER_LAYERING_RPMMD_REPOS)
    {
      g_clear_pointer (&self->final_revision, g_free);

      /* --- override/overlay and commit --- */
      if (!rpmostree_context_assemble (self->ctx, cancellable, error))
        return FALSE;
    }

  if (!rpmostree_rootfs_postprocess_common (self->tmprootfs_dfd, cancellable, error))
    return FALSE;

  if (rpmostree_origin_get_regenerate_initramfs (self->origin))
    {
      const char *const* add_dracut_argv = rpmostree_origin_get_initramfs_args (self->origin);

      rpmostree_output_task_begin ("Generating initramfs");

      g_autoptr(GVariant) kernel_state = rpmostree_find_kernel (self->tmprootfs_dfd, cancellable, error);
      if (!kernel_state)
        return FALSE;
      const char *bootdir;
      const char *kver;
      const char *kernel_path;
      const char *initramfs_path;
      g_variant_get (kernel_state, "(&s&s&sm&s)",
                     &kver, &bootdir,
                     &kernel_path, &initramfs_path);
      g_assert (initramfs_path);

      g_auto(GLnxTmpfile) initramfs_tmpf = { 0, };
      if (!rpmostree_run_dracut (self->tmprootfs_dfd, add_dracut_argv, kver,
                                 initramfs_path, NULL, &initramfs_tmpf,
                                 cancellable, error))
        return FALSE;

      if (!rpmostree_finalize_kernel (self->tmprootfs_dfd, bootdir, kver, kernel_path,
                                      &initramfs_tmpf, RPMOSTREE_FINALIZE_KERNEL_AUTO,
                                      cancellable, error))
        return FALSE;

      rpmostree_output_task_end ("done");
    }

  if (!rpmostree_context_commit (self->ctx, self->base_revision,
                                 RPMOSTREE_ASSEMBLE_TYPE_CLIENT_LAYERING,
                                 &self->final_revision, cancellable, error))
    return FALSE;

  /* Ensure we aren't holding any references to the tmpdir now that we're done;
   * rpmostree_sysroot_upgrader_deploy() eventually calls
   * rpmostree_syscore_cleanup() which deletes 🗑 the tmpdir.  See also similar
   * bits in the compose and container path.
   */
  g_clear_object (&self->ctx);
  glnx_close_fd (&self->tmprootfs_dfd);

  return TRUE;
}

static gboolean
requires_local_assembly (RpmOstreeSysrootUpgrader *self)
{
  /* Now, it's possible all requested packages are in the new tree, so we have
   * another optimization here for that case. This is a bit tricky: assuming we
   * came here from an 'rpm-ostree install', this might mean that we redeploy
   * the exact same base layer, with the only difference being the origin file.
   * We could down the line experiment with optimizing this by just updating the
   * merge deployment's origin.
   * https://github.com/projectatomic/rpm-ostree/issues/753
   */

  return self->overlay_packages->len > 0 ||
         self->override_remove_packages->len > 0 ||
         self->override_replace_local_packages->len > 0 ||
         g_hash_table_size (rpmostree_origin_get_local_packages (self->origin)) > 0 ||
         rpmostree_origin_get_regenerate_initramfs (self->origin);
}

/* Determine whether or not we require local modifications,
 * and if so, prepare layering (download rpm-md, depsolve etc).
 */
gboolean
rpmostree_sysroot_upgrader_prep_layering (RpmOstreeSysrootUpgrader *self,
                                          RpmOstreeSysrootUpgraderLayeringType *out_layering,
                                          gboolean                 *out_changed,
                                          GCancellable             *cancellable,
                                          GError                  **error)
{
  /* Default to no assembly required, and not changed */
  self->layering_initialized = TRUE;
  self->layering_type = RPMOSTREE_SYSROOT_UPGRADER_LAYERING_NONE;
  *out_layering = self->layering_type;
  *out_changed = FALSE;

  if (!rpmostree_origin_may_require_local_assembly (self->origin))
    {
      g_clear_pointer (&self->final_revision, g_free);
      /* No assembly? We're done then */
      return TRUE;
    }

  /* Do a bit more work to see whether or not we have to do assembly */
  if (!checkout_base_tree (self, cancellable, error))
    return FALSE;
  if (!finalize_overrides (self, cancellable, error))
    return FALSE;
  if (!finalize_overlays (self, cancellable, error))
    return FALSE;

  /* Recheck the state */
  if (!requires_local_assembly (self))
    {
      g_clear_pointer (&self->final_revision, g_free);
      /* No assembly? We're done then */
      return TRUE;
    }

  /* Actually do the prep work for local assembly */
  if (!prep_local_assembly (self, cancellable, error))
    return FALSE;

  *out_layering = self->layering_type;
  *out_changed = self->layering_changed;
  return TRUE;
}

gboolean
rpmostree_sysroot_upgrader_import_pkgs (RpmOstreeSysrootUpgrader *self,
                                        GCancellable             *cancellable,
                                        GError                  **error)
{
  g_assert (self->layering_initialized);
  g_assert (!self->pkgs_imported);
  self->pkgs_imported = TRUE;

  /* any layering actually required? */
  if (self->layering_type == RPMOSTREE_SYSROOT_UPGRADER_LAYERING_NONE)
    return TRUE;

  if (self->layering_type == RPMOSTREE_SYSROOT_UPGRADER_LAYERING_RPMMD_REPOS)
    {
      if (!rpmostree_context_download (self->ctx, cancellable, error))
        return FALSE;
      if (!rpmostree_context_import (self->ctx, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * rpmostree_sysroot_upgrader_deploy_set_kargs:
 * @self: Self
 * @kernel_args: A list of strings representing kernel_arguments
 * @cancellable: Cancellable
 * @error: Error
 *
 * A wrapper function that collects the kernel arguments then call the rpmostree_sysroot_upgrader_deploy
 * to write a pending deployment to the disk
 *
 * Returns: FALSE if rpmostree_sysroot_upgrader_deploy fails
 *
 */
gboolean
rpmostree_sysroot_upgrader_deploy_set_kargs (RpmOstreeSysrootUpgrader *self,
                                             char                    **kernel_args,
                                             GCancellable             *cancellable,
                                             GError                  **error)
{
  /* Set the attribute kargs directly, not sure whether or not to have a setter */
  /* Because .. currently pretty much here is the only place that we set it */
  self->kargs_strv = g_strdupv (kernel_args);
  return rpmostree_sysroot_upgrader_deploy (self, cancellable, error);
}

/**
 * rpmostree_sysroot_upgrader_deploy:
 * @self: Self
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write the new deployment to disk, overlay any packages requested, perform a
 * configuration merge with /etc, and update the bootloader configuration.
 */
gboolean
rpmostree_sysroot_upgrader_deploy (RpmOstreeSysrootUpgrader *self,
                                   GCancellable             *cancellable,
                                   GError                  **error)
{
  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN)
    {
      /* we already printed the transaction in do_final_local_assembly() */
      return TRUE;
    }

  /* Invoke prep_layering() if not done already */
  if (!self->layering_initialized)
    {
      RpmOstreeSysrootUpgraderLayeringType layering_type;
      gboolean layering_changed = FALSE;
      if (!rpmostree_sysroot_upgrader_prep_layering (self, &layering_type, &layering_changed,
                                                     cancellable, error))
        return FALSE;
    }

  if (!self->pkgs_imported)
    {
      if (!rpmostree_sysroot_upgrader_import_pkgs (self, cancellable, error))
        return FALSE;
    }

  /* Generate the final ostree commit */
  if (!perform_local_assembly (self, cancellable, error))
    return FALSE;

  /* make sure we have a known target to deploy */
  const char *target_revision = self->final_revision ?: self->base_revision;
  g_assert (target_revision);

  g_autoptr(GKeyFile) origin = rpmostree_origin_dup_keyfile (self->origin);
  g_autoptr(OstreeDeployment) new_deployment = NULL;
  if (!ostree_sysroot_deploy_tree (self->sysroot, self->osname,
                                   target_revision, origin,
                                   self->cfg_merge_deployment,
                                   self->kargs_strv,
                                   &new_deployment,
                                   cancellable, error))
    return FALSE;

  /* Also do a sanitycheck even if there's no local mutation; it's basically free
   * and might save someone in the future.  The RPMOSTREE_SKIP_SANITYCHECK
   * environment variable is just used by test-basic.sh currently.
   */
  if (!self->final_revision)
    {
      g_autofree char *deployment_path =
        ostree_sysroot_get_deployment_dirpath (self->sysroot, new_deployment);
      glnx_autofd int deployment_dfd = -1;
      if (!glnx_opendirat (ostree_sysroot_get_fd (self->sysroot), deployment_path, TRUE,
                           &deployment_dfd, error))
        return FALSE;

      if (!rpmostree_deployment_sanitycheck (deployment_dfd, cancellable, error))
        return FALSE;
    }

  if (self->final_revision)
    {
      /* Generate a temporary ref for the new deployment in case we are
       * interrupted; the base layer refs generation isn't transactional.
       */
      if (!ostree_repo_set_ref_immediate (self->repo, NULL, RPMOSTREE_TMP_BASE_REF,
                                          self->base_revision,
                                          cancellable, error))
        return FALSE;
    }

  if (!rpmostree_syscore_write_deployment (self->sysroot, new_deployment,
                                           self->cfg_merge_deployment, FALSE,
                                           cancellable, error))
    return FALSE;

  return TRUE;
}

GType
rpmostree_sysroot_upgrader_flags_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED",
          "ignore-unconfigured" },
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER",
          "allow-older" },
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN",
          "dry-run" }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("RpmOstreeSysrootUpgraderFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
