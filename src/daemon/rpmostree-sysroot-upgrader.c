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

#include "ostree-repo.h"

#define RPMOSTREE_TMP_BASE_REF "rpmostree/base/tmp"
#define RPMOSTREE_TMP_ROOTFS_DIR "extensions/rpmostree/commit"

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
  GHashTable *packages_to_add;
  GHashTable *packages_to_delete;

  /* Used during tree construction */
  OstreeRepoDevInoCache *devino_cache;
  int tmprootfs_dfd;
  char *tmprootfs;

  char *base_revision; /* Non-layered replicated commit */
  char *final_revision; /* Computed by layering; if NULL, only using base_revision */
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
parse_origin_keyfile (RpmOstreeSysrootUpgrader  *self,
                      GKeyFile                  *origin,
                      GCancellable           *cancellable,
                      GError                **error)
{
  g_clear_pointer (&self->origin, (GDestroyNotify)rpmostree_origin_unref);

  self->origin = rpmostree_origin_parse_keyfile (origin, error);
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

  return TRUE;
}

static gboolean
parse_origin_deployment (RpmOstreeSysrootUpgrader *self,
                         OstreeDeployment         *deployment,
                         GCancellable           *cancellable,
                         GError                **error)
{
  GKeyFile *origin = ostree_deployment_get_origin (deployment);
  if (!origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin known for deployment %s.%d",
                   ostree_deployment_get_csum (deployment),
                   ostree_deployment_get_deployserial (deployment));
      return FALSE;
    }
  return parse_origin_keyfile (self, origin, cancellable, error);
}

/* This is like ostree_sysroot_get_merge_deployment() except we explicitly
 * ignore the magical "booted" behavior. For rpm-ostree we're trying something
 * different now where we are a bit more stateful and pick up changes from the
 * pending root. This allows users to chain operations together naturally.
 */
static OstreeDeployment *
get_origin_merge_deployment (OstreeSysroot     *self,
                             const char        *osname)
{
  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (self);
  guint i;

  for (i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];

      if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
        continue;

      return g_object_ref (deployment);
  }

  return NULL;
}

static gboolean
rpmostree_sysroot_upgrader_initable_init (GInitable        *initable,
                                          GCancellable     *cancellable,
                                          GError          **error)
{
  gboolean ret = FALSE;
  RpmOstreeSysrootUpgrader *self = (RpmOstreeSysrootUpgrader*)initable;
  OstreeDeployment *booted_deployment =
    ostree_sysroot_get_booted_deployment (self->sysroot);
  const char *merge_deployment_csum = NULL;
  gboolean is_layered;

  if (booted_deployment == NULL && self->osname == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system and no OS specified");
      goto out;
    }

  if (self->osname == NULL)
    {
      g_assert (booted_deployment);
      self->osname = g_strdup (ostree_deployment_get_osname (booted_deployment));
    }
  else if (self->osname[0] == '\0')
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty osname");
      goto out;
    }

  if (!ostree_sysroot_get_repo (self->sysroot, &self->repo, cancellable, error))
    goto out;

  self->cfg_merge_deployment = ostree_sysroot_get_merge_deployment (self->sysroot, self->osname);
  self->origin_merge_deployment = get_origin_merge_deployment (self->sysroot, self->osname);
  if (self->cfg_merge_deployment == NULL || self->origin_merge_deployment == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No previous deployment for OS '%s'", self->osname);
      goto out;
    }

  /* Should we consider requiring --discard-hotfix here?
   * See also `ostree admin upgrade` bits.
   */
  if (!parse_origin_deployment (self, self->origin_merge_deployment, cancellable, error))
    goto out;

  merge_deployment_csum =
    ostree_deployment_get_csum (self->origin_merge_deployment);

  /* Now, load starting base/final checksums.  We may end up changing
   * one or both if we upgrade.  But we also want to support redeploying
   * without changing them.
   */
  if (!rpmostree_deployment_get_layered_info (self->repo,
                                              self->origin_merge_deployment,
                                              &is_layered, &self->base_revision,
                                              NULL, error))
    goto out;

  if (is_layered)
    /* base layer is already populated above */
    self->final_revision = g_strdup (merge_deployment_csum);
  else
    self->base_revision = g_strdup (merge_deployment_csum);

  g_assert (self->base_revision);

  self->packages_to_add = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, NULL);
  self->packages_to_delete = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, NULL);

  ret = TRUE;
 out:
  return ret;
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

  if (self->tmprootfs_dfd != -1)
    {
      (void)glnx_shutil_rm_rf_at (AT_FDCWD, self->tmprootfs, NULL, NULL);
      (void)close (self->tmprootfs_dfd);
    }
  g_free (self->tmprootfs);

  g_clear_pointer (&self->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->repo);
  g_free (self->osname);

  g_clear_object (&self->cfg_merge_deployment);
  g_clear_object (&self->origin_merge_deployment);
  g_clear_pointer (&self->origin, (GDestroyNotify)rpmostree_origin_unref);
  g_clear_pointer (&self->packages_to_add, (GDestroyNotify)g_hash_table_unref);
  g_clear_pointer (&self->packages_to_delete, (GDestroyNotify)g_hash_table_unref);
  g_free (self->base_revision);
  g_free (self->final_revision);

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

const char *const*
rpmostree_sysroot_upgrader_get_packages (RpmOstreeSysrootUpgrader *self)
{
  return rpmostree_origin_get_packages (self->origin);
}

OstreeDeployment*
rpmostree_sysroot_upgrader_get_merge_deployment (RpmOstreeSysrootUpgrader *self)
{
  return self->origin_merge_deployment;
}

static GHashTable*
hashset_from_strv (const char *const*strv)
{
  GHashTable *ht = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, NULL);
  for (char **it = (char**)strv; it && *it; it++)
    g_hash_table_add (ht, g_strdup (*it));
  return ht;
}

/**
 * rpmostree_sysroot_upgrader_add_packages:
 * @self: Self
 * @packages: Packages to add
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check that the @packages are not already requested and mark them for overlay.
 * */
gboolean
rpmostree_sysroot_upgrader_add_packages (RpmOstreeSysrootUpgrader *self,
                                         char                    **packages,
                                         GCancellable             *cancellable,
                                         GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) requested_packages =
    hashset_from_strv (rpmostree_origin_get_packages (self->origin));

  for (char **it = packages; it && *it; it++)
    {
      if (g_hash_table_contains (requested_packages, *it))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Package '%s' is already requested", *it);
          goto out;
        }
      g_hash_table_add (self->packages_to_add, g_strdup (*it));
    }

  ret = TRUE;
out:
  return ret;
}

/**
 * rpmostree_sysroot_upgrader_delete_packages:
 * @self: Self
 * @packages: Packages to delete
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check that the @packages were requested and remove them from overlay.
 */
gboolean
rpmostree_sysroot_upgrader_delete_packages (RpmOstreeSysrootUpgrader *self,
                                            char                    **packages,
                                            GCancellable             *cancellable,
                                            GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) requested_packages =
    hashset_from_strv (rpmostree_origin_get_packages (self->origin));

  for (char **it = packages; it && *it; it++)
    {
      if (!g_hash_table_contains (requested_packages, *it))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Package '%s' is not currently requested", *it);
          goto out;
        }
      g_hash_table_add (self->packages_to_delete, g_strdup (*it));
    }

  ret = TRUE;
out:
  return ret;
}

/*
 * Like ostree_sysroot_upgrader_pull(), but modified to handle layered packages.
 */
gboolean
rpmostree_sysroot_upgrader_pull (RpmOstreeSysrootUpgrader  *self,
                                 const char             *dir_to_pull,
                                 OstreeRepoPullFlags     flags,
                                 OstreeAsyncProgress    *progress,
                                 gboolean               *out_changed,
                                 GCancellable           *cancellable,
                                 GError                **error)
{
  gboolean ret = FALSE;
  char *refs_to_fetch[] = { NULL, NULL };
  g_autofree char *new_base_revision = NULL;
  g_autofree char *origin_remote = NULL;
  g_autofree char *origin_ref = NULL;

  if (!ostree_parse_refspec (rpmostree_origin_get_refspec (self->origin),
                             &origin_remote, 
                             &origin_ref,
                             error))
    goto out;

  if (rpmostree_origin_get_override_commit (self->origin) != NULL)
    refs_to_fetch[0] = (char*)rpmostree_origin_get_override_commit (self->origin);
  else
    refs_to_fetch[0] = origin_ref;

  g_assert (self->origin_merge_deployment);
  if (origin_remote)
    {
      if (!ostree_repo_pull_one_dir (self->repo, origin_remote, dir_to_pull, refs_to_fetch,
                                     flags, progress,
                                     cancellable, error))
        goto out;

      if (progress)
        ostree_async_progress_finish (progress);
    }

  if (rpmostree_origin_get_override_commit (self->origin) != NULL)
    {
      if (!ostree_repo_set_ref_immediate (self->repo,
                                          origin_remote,
                                          origin_ref,
                                          rpmostree_origin_get_override_commit (self->origin),
                                          cancellable,
                                          error))
        goto out;

      new_base_revision = g_strdup (rpmostree_origin_get_override_commit (self->origin));
    }
  else
    {
      if (!ostree_repo_resolve_rev (self->repo, rpmostree_origin_get_refspec (self->origin), FALSE,
                                    &new_base_revision, error))
        goto out;
    }

  {
    gboolean allow_older =
      (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER) > 0;
    gboolean changed = FALSE;

    if (strcmp (new_base_revision, self->base_revision) != 0)
      {
        changed = TRUE;

        if (!allow_older)
          {
            if (!ostree_sysroot_upgrader_check_timestamps (self->repo, self->base_revision,
                                                           new_base_revision,
                                                           error))
              goto out;
          }

        g_free (self->base_revision);
        self->base_revision = g_steal_pointer (&new_base_revision);
      }

    *out_changed = changed;
  }

  ret = TRUE;
 out:
  return ret;
}

/* update the origin with the new packages */
static gboolean
update_requested_packages (RpmOstreeSysrootUpgrader *self,
                           GHashTable               *pkgset,
                           GCancellable             *cancellable,
                           GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GKeyFile) new_origin = rpmostree_origin_dup_keyfile (self->origin);
  glnx_free char **pkgv =
    (char**) g_hash_table_get_keys_as_array (pkgset, NULL);

  g_key_file_set_string_list (new_origin, "packages", "requested",
                              (const char* const*) pkgv, g_strv_length(pkgv));

  /* migrate to baserefspec model if necessary */
  g_key_file_set_value (new_origin, "origin", "baserefspec",
                        rpmostree_origin_get_refspec (self->origin));
  if (!g_key_file_remove_key (new_origin, "origin", "refspec", error))
    {
      if (g_error_matches (*error, G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        g_clear_error (error);
      else
        goto out;
    }
  g_clear_pointer (&self->origin, (GDestroyNotify)rpmostree_origin_unref);
  if (!parse_origin_keyfile (self, new_origin, cancellable, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

static gboolean
checkout_base_tree (RpmOstreeSysrootUpgrader *self,
                    GCancellable          *cancellable,
                    GError               **error)
{
  OstreeRepoCheckoutAtOptions checkout_options = { 0, };
  int repo_dfd = ostree_repo_get_dfd (self->repo); /* borrowed */

  g_assert (!self->tmprootfs);
  g_assert_cmpint (self->tmprootfs_dfd, ==, -1);

  /* let's give the user some feedback so they don't think we're blocked */
  rpmostree_output_task_begin ("Checking out tree %.7s", self->base_revision);

  if (!glnx_shutil_mkdir_p_at (repo_dfd,
                               dirname (strdupa (RPMOSTREE_TMP_ROOTFS_DIR)),
                               0755, cancellable, error))
    return FALSE;

  /* delete dir in case a previous run didn't finish successfully */
  if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR,
                             cancellable, error))
    return FALSE;

  /* NB: we let ostree create the dir for us so that the root dir has the
   * correct xattrs (e.g. selinux label) */
  checkout_options.devino_to_csum_cache = self->devino_cache;
  if (!ostree_repo_checkout_at (self->repo, &checkout_options,
                                repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR,
                                self->base_revision, cancellable, error))
    return FALSE;

  /* Now we'll delete it on cleanup */
  self->tmprootfs = glnx_fdrel_abspath (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR);

  if (!glnx_opendirat (repo_dfd, self->tmprootfs, FALSE, &self->tmprootfs_dfd, error))
    return FALSE;

  rpmostree_output_task_end ("done");

  return TRUE;
}

/* XXX: This is ugly, but the alternative is to de-couple RpmOstreeTreespec from
 * RpmOstreeContext, which also use it for hashing and store it directly in
 * assembled commit metadata. Probably assemble_commit() should live somewhere
 * else, maybe directly in `container-builtins.c`. */
static RpmOstreeTreespec *
generate_treespec (GHashTable *packages)
{
  g_autoptr(RpmOstreeTreespec) ret = NULL;
  g_autoptr(GError) tmp_error = NULL;
  g_autoptr(GKeyFile) treespec = g_key_file_new ();
  glnx_free char **pkgv = /* NB: don't use g_strv_free() -- the keys belong to the table */
    (char**) g_hash_table_get_keys_as_array (packages, NULL);

  g_key_file_set_string_list (treespec, "tree", "packages",
                              (const char* const*) pkgv, g_strv_length(pkgv));

  ret = rpmostree_treespec_new_from_keyfile (treespec, &tmp_error);
  g_assert_no_error (tmp_error);

  return g_steal_pointer (&ret);
}

/* Given a rootfs containing an rpmdb and a list of packages, calls back for
 * each pkg given found in the db. */
/* XXX: move to a utility file? */
static gboolean
find_pkgs_in_rpmdb (int rootfs_dfd, GHashTable *pkgs,
                    gboolean (*callback) (GHashTableIter *it,
                                          const char *pkg,
                                          GError **error,
                                          gpointer opaque),
                    GCancellable *cancellable,
                    gpointer opaque,
                    GError **error)
{
  gboolean ret = FALSE;
  GHashTableIter it;
  gpointer itkey;
  g_autoptr(RpmOstreeRefSack) rsack = NULL;

  rsack = rpmostree_get_refsack_for_root (rootfs_dfd, ".", cancellable, error);
  if (rsack == NULL)
    goto out;

  /* search for each package */
  g_hash_table_iter_init (&it, pkgs);
  while (g_hash_table_iter_next (&it, &itkey, NULL))
    {
      g_autoptr(GPtrArray) pkglist = NULL;
      hy_autoquery HyQuery query = hy_query_create (rsack->sack);
      hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, itkey);
      pkglist = hy_query_run (query);

      /* did we find the package? */
      if (pkglist->len != 0)
        if (!callback (&it, itkey, error, opaque))
          goto out;
    }

  ret = TRUE;
out:
  return ret;
}

static gboolean
pkg_find_cb (GHashTableIter *it,
             const char *pkg,
             GError **error,
             gpointer opaque)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (opaque);

  /* did the user explicitly request this package during this session? */
  if (g_hash_table_contains (self->packages_to_add, pkg))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "package '%s' is already in the deployment", pkg);
      return FALSE;
    }

  g_print ("Note: package '%s' is already in the deployment; "
           "it will no longer be layered.\n", pkg);

  g_hash_table_iter_remove (it);

  return TRUE;
}

/* Do a partial checkout of the rpmdb, and given requested_packages and
 * packages_to_add and packages_to_delete, update requested_packages to reflect
 * the final set of packages to actually overlay. */
static gboolean
finalize_requested_packages (RpmOstreeSysrootUpgrader *self,
                             GCancellable             *cancellable,
                             GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) pkgset =
    hashset_from_strv (rpmostree_origin_get_packages (self->origin));

  /* remove packages_to_delete from the set */
  { GHashTableIter it;
    gpointer itkey;
    g_hash_table_iter_init (&it, self->packages_to_delete);
    while (g_hash_table_iter_next (&it, &itkey, NULL))
      g_hash_table_remove (pkgset, g_strdup (itkey));
  }

  /* add packages_to_add to the set */
  { GHashTableIter it;
    gpointer itkey;
    g_hash_table_iter_init (&it, self->packages_to_add);
    while (g_hash_table_iter_next (&it, &itkey, NULL))
      g_hash_table_add (pkgset, g_strdup (itkey));
  }

  if (!find_pkgs_in_rpmdb (self->tmprootfs_dfd, pkgset, pkg_find_cb,
                           cancellable, self, error))
    goto out;

  if (!update_requested_packages (self, pkgset, cancellable, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

static gboolean
get_pkgcache_repo (OstreeRepo   *parent,
                   OstreeRepo  **out_pkgcache,
                   GCancellable *cancellable,
                   GError      **error)
{
  gboolean ret = FALSE;
  glnx_unref_object OstreeRepo *pkgcache = NULL;
  g_autoptr(GFile) pkgcache_path = NULL;

  /* get the GFile to it */
  {
    int parent_dfd = ostree_repo_get_dfd (parent); /* borrowed */
    g_autofree char *pkgcache_path_s =
      glnx_fdrel_abspath (parent_dfd, "extensions/rpmostree/pkgcache");
    pkgcache_path = g_file_new_for_path (pkgcache_path_s);
  }

  pkgcache = ostree_repo_new (pkgcache_path);

  if (!g_file_query_exists (pkgcache_path, cancellable))
    {
      g_autoptr(GKeyFile) config = NULL;
      GFile *parent_path = ostree_repo_get_path (parent);

      if (!g_file_make_directory_with_parents (pkgcache_path,
                                               cancellable, error))
        goto out;

      if (!ostree_repo_create (pkgcache, OSTREE_REPO_MODE_BARE,
                               cancellable, error))
        goto out;

      config = ostree_repo_copy_config (pkgcache);
      g_key_file_set_string (config, "core", "parent",
                             gs_file_get_path_cached (parent_path));
      ostree_repo_write_config (pkgcache, config, error);

      /* yuck... ostree already opened the repo when we did create, but that was
       * before we had the parent repo set in its config. there's no way to
       * "reload" the config, so let's just tear down and recreate for now */
      g_clear_object (&pkgcache);
      pkgcache = ostree_repo_new (pkgcache_path);
    }

  if (!ostree_repo_open (pkgcache, cancellable, error))
    goto out;

  *out_pkgcache = g_steal_pointer (&pkgcache);

  ret = TRUE;
out:
  return ret;
}

static gboolean
overlay_final_pkgset (RpmOstreeSysrootUpgrader *self,
                      GCancellable             *cancellable,
                      GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(RpmOstreeContext) ctx = NULL;
  g_autoptr(RpmOstreeTreespec) treespec = NULL;
  g_autoptr(RpmOstreeInstall) install = {0,};
  g_autoptr(GHashTable) pkgset = hashset_from_strv (rpmostree_origin_get_packages (self->origin));
  g_autofree char *tmprootfs_abspath = glnx_fdrel_abspath (self->tmprootfs_dfd, ".");
  const gboolean have_packages = g_hash_table_size (pkgset) > 0;
  glnx_unref_object OstreeRepo *pkgcache_repo = NULL;

  ctx = rpmostree_context_new_system (cancellable, error);

  /* pass merge deployment related values */
  {
    DnfContext *hifctx = rpmostree_context_get_hif (ctx);
    g_autofree char *sysroot_path =
      g_file_get_path (ostree_sysroot_get_path (self->sysroot));
    g_autofree char *merge_deployment_dirpath =
      ostree_sysroot_get_deployment_dirpath (self->sysroot,
                                             self->cfg_merge_deployment);
    g_autofree char *merge_deployment_root =
      g_build_filename (sysroot_path, merge_deployment_dirpath, NULL);
    g_autofree char *reposdir = g_build_filename (merge_deployment_root,
                                                  "etc/yum.repos.d", NULL);
    g_autofree char *passwddir = g_build_filename (merge_deployment_root,
                                                  "etc", NULL);

    /* point libhif to the yum.repos.d and os-release of the merge deployment */
    dnf_context_set_repo_dir (hifctx, reposdir);
    dnf_context_set_source_root (hifctx, tmprootfs_abspath);

    /* point the core to the passwd & group of the merge deployment */
    rpmostree_context_set_passwd_dir (ctx, passwddir);
  }

  /* load the sepolicy to use during import */
  {
    glnx_unref_object OstreeSePolicy *sepolicy = NULL;
    if (!rpmostree_prepare_rootfs_get_sepolicy (self->tmprootfs_dfd, ".", &sepolicy,
                                                cancellable, error))
      goto out;

    rpmostree_context_set_sepolicy (ctx, sepolicy);
  }

  /* NB: We're pretty much using the defaults for the other treespec values like
   * instlang and docs since it would be hard to expose the cli for them because
   * they wouldn't affect just the new pkgs, but even previously added ones. */
  treespec = generate_treespec (pkgset);
  if (treespec == NULL)
    goto out;

  if (!rpmostree_context_setup (ctx, tmprootfs_abspath, NULL, treespec,
                                cancellable, error))
    goto out;

  if (!get_pkgcache_repo (self->repo, &pkgcache_repo, cancellable, error))
    goto out;

  rpmostree_context_set_repos (ctx, self->repo, pkgcache_repo);

  if (have_packages)
    {
      /* --- Downloading metadata --- */
      if (!rpmostree_context_download_metadata (ctx, cancellable, error))
        goto out;

      /* --- Resolving dependencies --- */
      if (!rpmostree_context_prepare_install (ctx, &install, cancellable, error))
        goto out;
    }
  else
    rpmostree_context_set_is_empty (ctx);

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN)
    {
      g_assert (have_packages);
      rpmostree_print_transaction (rpmostree_context_get_hif (ctx));
      ret = TRUE;
      goto out;
    }

  if (have_packages)
    {
      /* --- Download as necessary --- */
      if (!rpmostree_context_download (ctx, install, cancellable, error))
        goto out;

      /* --- Import as necessary --- */
      if (!rpmostree_context_import (ctx, install, cancellable, error))
        goto out;

      /* --- Relabel as necessary --- */
      if (!rpmostree_context_relabel (ctx, install, cancellable, error))
        goto out;

      /* --- Overlay and commit --- */

      g_clear_pointer (&self->final_revision, g_free);
      if (!rpmostree_context_assemble_tmprootfs (ctx, self->tmprootfs_dfd, self->devino_cache,
                                                 RPMOSTREE_ASSEMBLE_TYPE_CLIENT_LAYERING,
                                                 (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_NOSCRIPTS) > 0,
                                                 cancellable, error))
        goto out;
    }

  if (!rpmostree_rootfs_postprocess_common (self->tmprootfs_dfd, cancellable, error))
    goto out;

  if (rpmostree_origin_get_regenerate_initramfs (self->origin))
    {
      glnx_fd_close int initramfs_tmp_fd = -1;
      g_autofree char *initramfs_tmp_path = NULL;
      const char *bootdir;
      const char *kver;
      const char *kernel_path;
      const char *initramfs_path;
      g_autoptr(GVariant) kernel_state = NULL;
      g_auto(GStrv) add_dracut_argv = NULL;

      add_dracut_argv = rpmostree_origin_get_initramfs_args (self->origin);

      rpmostree_output_task_begin ("Generating initramfs");

      kernel_state = rpmostree_find_kernel (self->tmprootfs_dfd, cancellable, error);
      if (!kernel_state)
        goto out;
      g_variant_get (kernel_state, "(&s&s&sm&s)",
                     &kver, &bootdir,
                     &kernel_path, &initramfs_path);
      g_assert (initramfs_path);

      if (!rpmostree_run_dracut (self->tmprootfs_dfd, add_dracut_argv, initramfs_path,
                                 &initramfs_tmp_fd, &initramfs_tmp_path,
                                 cancellable, error))
        goto out;

      if (!rpmostree_finalize_kernel (self->tmprootfs_dfd, bootdir, kver,
                                      kernel_path,
                                      initramfs_tmp_path, initramfs_tmp_fd,
                                      cancellable, error))
        goto out;

      rpmostree_output_task_end ("done");
    }

  if (!rpmostree_context_commit_tmprootfs (ctx, self->tmprootfs_dfd, self->devino_cache,
                                           self->base_revision,
                                           RPMOSTREE_ASSEMBLE_TYPE_CLIENT_LAYERING,
                                           &self->final_revision,
                                           cancellable, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

static gboolean
do_local_assembly (RpmOstreeSysrootUpgrader *self,
                   GCancellable             *cancellable,
                   GError                  **error)
{
  self->devino_cache = ostree_repo_devino_cache_new ();

  if (!checkout_base_tree (self, cancellable, error))
    return FALSE;

  /* check if there are any items in requested_packages or pkgs_to_add that are
   * already installed in base_rev */
  if (!finalize_requested_packages (self, cancellable, error))
    return FALSE;

  /* Now, it's possible all requested packages are in the new tree (or
   * that the user wants to stop overlaying them), so we have another
   * optimization here for that case.
   */
  if (!rpmostree_origin_get_regenerate_initramfs (self->origin) &&
      g_strv_length ((char**)rpmostree_origin_get_packages (self->origin)) == 0)
    {
      g_clear_pointer (&self->final_revision, g_free);
    }
  else
    {
      if (!overlay_final_pkgset (self, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* For each deployment, if they are layered deployments, then create a ref
 * pointing to their bases. This is mostly to work around ostree's auto-ref
 * cleanup. Otherwise we might get into a situation where after the origin ref
 * is updated, we lose our parent, which means that users can no longer
 * add/delete packages on that deployment. (They can always just re-pull it, but
 * let's try to be nice).
 **/
static gboolean
generate_baselayer_refs (OstreeSysroot            *sysroot,
                         OstreeRepo               *repo,
                         GCancellable             *cancellable,
                         GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) refs = NULL;
  g_autoptr(GHashTable) bases =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (!ostree_repo_list_refs_ext (repo, "rpmostree/base", &refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE,
                                  cancellable, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  /* delete all the refs */
  {
    GHashTableIter it;
    gpointer key;

    g_hash_table_iter_init (&it, refs);
    while (g_hash_table_iter_next (&it, &key, NULL))
      {
        const char *ref = key;
        ostree_repo_transaction_set_refspec (repo, ref, NULL);
      }
  }

  /* collect the csums */
  {
    guint i = 0;
    g_autoptr(GPtrArray) deployments =
      ostree_sysroot_get_deployments (sysroot);

    /* existing deployments */
    for (; i < deployments->len; i++)
      {
        OstreeDeployment *deployment = deployments->pdata[i];
        g_autofree char *base_rev = NULL;

        if (!rpmostree_deployment_get_layered_info (repo, deployment, NULL,
                                                    &base_rev, NULL, error))
          goto out;

        if (base_rev)
          g_hash_table_add (bases, g_steal_pointer (&base_rev));
      }
  }

  /* create the new refs */
  {
    guint i = 0;
    GHashTableIter it;
    gpointer key;

    g_hash_table_iter_init (&it, bases);
    while (g_hash_table_iter_next (&it, &key, NULL))
      {
        const char *base = key;
        g_autofree char *ref = g_strdup_printf ("rpmostree/base/%u", i++);
        ostree_repo_transaction_set_refspec (repo, ref, base);
      }
  }

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}

/* For all packages in the sack, generate a cached refspec and add it
 * to @referenced_pkgs. This is necessary to implement garbage
 * collection of layered package refs.
 */
static gboolean
add_package_refs_to_set (RpmOstreeRefSack *rsack,
                         GHashTable *referenced_pkgs,
                         GCancellable *cancellable,
                         GError **error)
{
  g_autoptr(GPtrArray) pkglist = NULL;
  hy_autoquery HyQuery query = hy_query_create (rsack->sack);
  hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  pkglist = hy_query_run (query);

  /* TODO: convert this to an iterator to avoid lots of malloc */

  if (pkglist->len == 0)
    sd_journal_print (LOG_WARNING, "Failed to find any packages in root");
  else
    {
      for (guint i = 0; i < pkglist->len; i++)
        {
          DnfPackage *pkg = pkglist->pdata[i];
          g_autofree char *pkgref = rpmostree_get_cache_branch_pkg (pkg);
          g_hash_table_add (referenced_pkgs, g_steal_pointer (&pkgref));
        }
    }

  return TRUE;
}

/* Loop over all deployments, gathering all referenced NEVRAs for
 * layered packages.  Then delete any cached pkg refs that aren't in
 * that set.
 */
static gboolean
clean_pkgcache_orphans (OstreeSysroot            *sysroot,
                        OstreeRepo               *repo,
                        GCancellable             *cancellable,
                        GError                  **error)
{
  glnx_unref_object OstreeRepo *pkgcache_repo = NULL;
  g_autoptr(GPtrArray) deployments =
    ostree_sysroot_get_deployments (sysroot);
  g_autoptr(GHashTable) current_refs = NULL;
  g_autoptr(GHashTable) referenced_pkgs = /* cache refs of packages we want to keep */
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GHashTableIter hiter;
  gpointer hkey, hvalue;
  gint n_objects_total;
  gint n_objects_pruned;
  guint64 freed_space;
  guint n_freed = 0;

  if (!get_pkgcache_repo (repo, &pkgcache_repo, cancellable, error))
    return FALSE;

  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      gboolean is_layered;

      if (!rpmostree_deployment_get_layered_info (repo, deployment, &is_layered,
                                                  NULL, NULL, error))
        return FALSE;

      if (is_layered)
        {
          g_autoptr(RpmOstreeRefSack) rsack = NULL;
          g_autofree char *deployment_dirpath = NULL;

          deployment_dirpath = ostree_sysroot_get_deployment_dirpath (sysroot, deployment);

          /* We could do this via the commit object, but it's faster
           * to reuse the existing rpmdb checkout.
           */
          rsack = rpmostree_get_refsack_for_root (ostree_sysroot_get_fd (sysroot),
                                                  deployment_dirpath,
                                                  cancellable, error);
          if (rsack == NULL)
            return FALSE;

          if (!add_package_refs_to_set (rsack, referenced_pkgs,
                                        cancellable, error))
            return FALSE;
        }
    }

  if (!ostree_repo_list_refs_ext (pkgcache_repo, "rpmostree/pkg", &current_refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE,
                                  cancellable, error))
    return FALSE;

  g_hash_table_iter_init (&hiter, current_refs);
  while (g_hash_table_iter_next (&hiter, &hkey, &hvalue))
    {
      const char *ref = hkey;
      if (g_hash_table_contains (referenced_pkgs, ref))
        continue;

      if (!ostree_repo_set_ref_immediate (pkgcache_repo, NULL, ref, NULL,
                                          cancellable, error))
        return FALSE;
      n_freed++;
    }

  if (!ostree_repo_prune (pkgcache_repo, OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY, 0,
                          &n_objects_total, &n_objects_pruned, &freed_space,
                          cancellable, error))
    return FALSE;

  if (n_freed > 0 || freed_space > 0)
    {
      char *freed_space_str = g_format_size_full (freed_space, 0);
      g_print ("Freed pkgcache branches: %u size: %s\n", n_freed, freed_space_str);
    }

  return TRUE;
}

/* Clean up to match the current deployments. This used to be a private static,
 * but is now used by the cleanup txn.
 */
gboolean
rpmostree_sysroot_upgrader_cleanup (OstreeSysroot            *sysroot,
                                    OstreeRepo               *repo,
                                    GCancellable             *cancellable,
                                    GError                  **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo); /* borrowed */

  /* regenerate the baselayer refs in case we just kicked out an ancient layered
   * deployment whose base layer is not needed anymore */
  if (!generate_baselayer_refs (sysroot, repo, cancellable, error))
    return FALSE;

  /* Delete our temporary ref */
  if (!ostree_repo_set_ref_immediate (repo, NULL, RPMOSTREE_TMP_BASE_REF,
                                      NULL, cancellable, error))
    return FALSE;

  /* and shake it loose */
  if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
    return FALSE;

  if (!clean_pkgcache_orphans (sysroot, repo, cancellable, error))
    return FALSE;

  /* delete our checkout dir in case a previous run didn't finish
     successfully */
  if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR,
                             cancellable, error))
    return FALSE;

  return TRUE;
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
  gboolean ret = FALSE;
  glnx_unref_object OstreeDeployment *new_deployment = NULL;
  const char *target_revision;
  g_autoptr(GKeyFile) origin = NULL;

  if (rpmostree_origin_get_regenerate_initramfs (self->origin) ||
      g_strv_length ((gchar**)rpmostree_origin_get_packages (self->origin)) > 0 ||
      (g_hash_table_size (self->packages_to_add) > 0) ||
      (g_hash_table_size (self->packages_to_delete) > 0))
    {
      if (!do_local_assembly (self, cancellable, error))
        goto out;
    }
  else
    g_clear_pointer (&self->final_revision, g_free);

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN)
    {
      ret = TRUE;
      goto out;
    }

  /* make sure we have a known target to deploy */
  target_revision = self->final_revision ?: self->base_revision;
  g_assert (target_revision);

  origin = rpmostree_origin_dup_keyfile (self->origin);
  if (!ostree_sysroot_deploy_tree (self->sysroot, self->osname,
                                   target_revision, origin,
                                   self->cfg_merge_deployment,
                                   NULL,
                                   &new_deployment,
                                   cancellable, error))
    goto out;

  if (self->final_revision)
    {
      /* Generate a temporary ref for the new deployment in case we are
       * interrupted; the base layer refs generation isn't transactional.
       */
      if (!ostree_repo_set_ref_immediate (self->repo, NULL, RPMOSTREE_TMP_BASE_REF,
                                          self->base_revision,
                                          cancellable, error))
        goto out;
    }

  if (!ostree_sysroot_simple_write_deployment (self->sysroot, self->osname,
                                               new_deployment,
                                               self->cfg_merge_deployment,
                                               OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NO_CLEAN,
                                               cancellable, error))
    goto out;

  if (!rpmostree_sysroot_upgrader_cleanup (self->sysroot, self->repo, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
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
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN",
          "pkgoverlay-dry-run" }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("RpmOstreeSysrootUpgraderFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
