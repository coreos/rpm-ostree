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
#include "rpmostreed-utils.h"
#include "rpmostree-util.h"

#include "rpmostree-sysroot-upgrader.h"
#include "rpmostree-core.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-output.h"

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
  char *osname;
  RpmOstreeSysrootUpgraderFlags flags;

  OstreeDeployment *merge_deployment;
  GKeyFile *origin;
  char *origin_refspec;
  char **requested_packages;
  GHashTable *packages_to_add;
  GHashTable *packages_to_delete;
  char *override_csum;

  char *new_revision;
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
parse_refspec (RpmOstreeSysrootUpgrader  *self,
               GCancellable           *cancellable,
               GError                **error)
{
  gboolean ret = FALSE;
  g_autofree char *unconfigured_state = NULL;
  g_autofree char *csum = NULL;

  if ((self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED) == 0)
    {
      /* If explicit action by the OS creator is requried to upgrade, print their text as an error */
      unconfigured_state = g_key_file_get_string (self->origin, "origin", "unconfigured-state", NULL);
      if (unconfigured_state)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "origin unconfigured-state: %s", unconfigured_state);
          goto out;
        }
    }

  g_clear_pointer (&self->requested_packages, g_strfreev);
  g_clear_pointer (&self->origin_refspec, g_free);

  if (!_rpmostree_util_parse_origin (self->origin, &self->origin_refspec,
                                     &self->requested_packages, error))
    goto out;

  /* it's just easier to make it a proper empty list than to check for NULL
   * everytime */
  if (self->requested_packages == NULL)
    self->requested_packages = g_new0 (gchar *, 1);

  csum = g_key_file_get_string (self->origin, "origin", "override-commit", NULL);
  if (csum != NULL && !ostree_validate_checksum_string (csum, error))
    goto out;
  self->override_csum = g_steal_pointer (&csum);

  ret = TRUE;
 out:
  return ret;
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

  self->merge_deployment = ostree_sysroot_get_merge_deployment (self->sysroot, self->osname); 
  if (self->merge_deployment == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No previous deployment for OS '%s'", self->osname);
      goto out;
    }

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_REDEPLOY)
    self->new_revision =
      g_strdup (ostree_deployment_get_csum (self->merge_deployment));

  self->origin = NULL;
  {
    GKeyFile *original_origin = /* I just had to use that name */
      ostree_deployment_get_origin (self->merge_deployment);
    if (original_origin)
      self->origin = _rpmostree_util_keyfile_clone (original_origin);
  }

  if (!self->origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin known for deployment %s.%d",
                   ostree_deployment_get_csum (self->merge_deployment),
                   ostree_deployment_get_deployserial (self->merge_deployment));
      goto out;
    }
  g_key_file_ref (self->origin);

  self->packages_to_add = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, NULL);
  self->packages_to_delete = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, NULL);

  if (!parse_refspec (self, cancellable, error))
    goto out;

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

  g_clear_object (&self->sysroot);
  g_free (self->osname);

  g_clear_object (&self->merge_deployment);
  if (self->origin)
    g_key_file_unref (self->origin);
  g_free (self->origin_refspec);
  g_strfreev (self->requested_packages);
  g_hash_table_unref (self->packages_to_add);
  g_hash_table_unref (self->packages_to_delete);
  g_free (self->override_csum);
  g_free (self->new_revision);

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

/**
 * rpmostree_sysroot_upgrader_get_origin:
 * @self: Sysroot
 *
 * Returns: (transfer none): The origin file, or %NULL if unknown
 */
GKeyFile *
rpmostree_sysroot_upgrader_get_origin (RpmOstreeSysrootUpgrader *self)
{
  return self->origin;
}

/**
 * ostree_sysroot_upgrader_dup_origin:
 * @self: Sysroot
 *
 * Returns: (transfer full): A copy of the origin file, or %NULL if unknown
 */
GKeyFile *
rpmostree_sysroot_upgrader_dup_origin (RpmOstreeSysrootUpgrader *self)
{
  GKeyFile *copy = NULL;

  g_return_val_if_fail (RPMOSTREE_IS_SYSROOT_UPGRADER (self), NULL);

  if (self->origin != NULL)
    {
      g_autofree char *data = NULL;
      gsize length = 0;

      copy = g_key_file_new ();
      data = g_key_file_to_data (self->origin, &length, NULL);
      g_key_file_load_from_data (copy, data, length,
                                 G_KEY_FILE_KEEP_COMMENTS, NULL);
    }

  return copy;
}

/**
 * ostree_sysroot_upgrader_set_origin:
 * @self: Sysroot
 * @origin: (allow-none): The new origin
 * @cancellable: Cancellable
 * @error: Error
 *
 * Replace the origin with @origin.
 */
gboolean
rpmostree_sysroot_upgrader_set_origin (RpmOstreeSysrootUpgrader *self,
                                       GKeyFile              *origin,
                                       GCancellable          *cancellable,
                                       GError               **error)
{
  gboolean ret = FALSE;

  g_clear_pointer (&self->origin, g_key_file_unref);
  if (origin)
    {
      self->origin = g_key_file_ref (origin);
      if (!parse_refspec (self, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/* updates an origin's refspec without migrating format */
static gboolean
origin_set_refspec (GKeyFile   *origin,
                    const char *new_refspec,
                    GError    **error)
{
  if (g_key_file_has_key (origin, "origin", "baserefspec", error))
    {
      g_key_file_set_value (origin, "origin", "baserefspec", new_refspec);
      return TRUE;
    }

  if (error && *error)
    return FALSE;

  g_key_file_set_value (origin, "origin", "refspec", new_refspec);
  return TRUE;
}

gboolean
rpmostree_sysroot_upgrader_set_origin_rebase (RpmOstreeSysrootUpgrader *self,
                                              const char *new_refspec,
                                              GError **error)
{
  g_autoptr(GKeyFile) new_origin = rpmostree_sysroot_upgrader_dup_origin (self);

  if (!origin_set_refspec (new_origin, new_refspec, error))
    return FALSE;

  g_clear_pointer (&self->origin, g_key_file_unref);
  self->origin = g_key_file_ref (new_origin);

  /* this will update self->origin_refspec */
  if (!parse_refspec (self, NULL, error))
    return FALSE;

  return TRUE;
}

void
rpmostree_sysroot_upgrader_set_origin_override (RpmOstreeSysrootUpgrader *self,
                                                const char *override_commit)
{
  if (override_commit != NULL)
    g_key_file_set_string (self->origin, "origin", "override-commit", override_commit);
  else
    g_key_file_remove_key (self->origin, "origin", "override_commit", NULL);
}

const char *
rpmostree_sysroot_upgrader_get_refspec (RpmOstreeSysrootUpgrader *self)
{
  return self->origin_refspec;
}

const char *const*
rpmostree_sysroot_upgrader_get_packages (RpmOstreeSysrootUpgrader *self)
{
  return (const char * const *)self->requested_packages;
}

OstreeDeployment*
rpmostree_sysroot_upgrader_get_merge_deployment (RpmOstreeSysrootUpgrader *self)
{
  return self->merge_deployment;
}

/**
 * rpmostree_sysroot_upgrader_get_origin_description:
 * @self: Upgrader
 *
 * Returns: A one-line descriptive summary of the origin, or %NULL if unknown
 */
char *
rpmostree_sysroot_upgrader_get_origin_description (RpmOstreeSysrootUpgrader *self)
{
  return g_strdup (rpmostree_sysroot_upgrader_get_refspec (self));
}

static GHashTable*
hashset_from_strv (char **strv)
{
  GHashTable *ht = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, NULL);
  for (char **it = strv; it && *it; it++)
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
    hashset_from_strv (self->requested_packages);

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
    hashset_from_strv (self->requested_packages);

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

static gboolean
commit_get_parent_csum (OstreeRepo  *repo,
                        const char  *child,
                        char       **out_csum,
                        GError     **error)
{
  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, child, &commit, NULL, error))
    return FALSE;
  *out_csum = ostree_commit_get_parent (commit);
  return TRUE;
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
  glnx_unref_object OstreeRepo *repo = NULL;
  char *refs_to_fetch[] = { NULL, NULL };
  const char *from_revision = NULL;
  g_autofree char *new_revision = NULL;
  g_autofree char *origin_remote = NULL;
  g_autofree char *origin_ref = NULL;

  if (!ostree_parse_refspec (self->origin_refspec,
                             &origin_remote, 
                             &origin_ref,
                             error))
    goto out;

  if (self->override_csum != NULL)
    refs_to_fetch[0] = self->override_csum;
  else
    refs_to_fetch[0] = origin_ref;

  if (!ostree_sysroot_get_repo (self->sysroot, &repo, cancellable, error))
    goto out;

  g_assert (self->merge_deployment);
  from_revision = ostree_deployment_get_csum (self->merge_deployment);

  if (origin_remote)
    {
      if (!ostree_repo_pull_one_dir (repo, origin_remote, dir_to_pull, refs_to_fetch,
                                     flags, progress,
                                     cancellable, error))
        goto out;

      if (progress)
        ostree_async_progress_finish (progress);
    }

  if (self->override_csum != NULL)
    {
      if (!ostree_repo_set_ref_immediate (repo,
                                          origin_remote,
                                          origin_ref,
                                          self->override_csum,
                                          cancellable,
                                          error))
        goto out;

      self->new_revision = g_strdup (self->override_csum);
    }
  else
    {
      if (!ostree_repo_resolve_rev (repo, self->origin_refspec, FALSE,
                                    &self->new_revision, error))
        goto out;

    }

  {
    gboolean allow_older =
      (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER) > 0;
    const char *compare_rev = from_revision;
    g_autofree char *base_rev = NULL;
    gboolean changed = FALSE;

    if (from_revision)
      {
        /* if there are pkgs layered on the from rev, then we should compare
         * the parent instead, which is the 'base' layer */
        if (g_strv_length (self->requested_packages) > 0)
          {
            if (!commit_get_parent_csum (repo, from_revision, &base_rev, error))
              goto out;
            g_assert (base_rev);
            compare_rev = base_rev;
          }
      }

    if (g_strcmp0 (compare_rev, self->new_revision) != 0)
      changed = TRUE;

    if (changed && compare_rev && !allow_older)
      if (!ostree_sysroot_upgrader_check_timestamps (repo, compare_rev,
                                                     self->new_revision,
                                                     error))
        goto out;

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
  glnx_free char **pkgv =
    (char**) g_hash_table_get_keys_as_array (pkgset, NULL);

  g_key_file_set_string_list (self->origin, "packages", "requested",
                              (const char* const*) pkgv, g_strv_length(pkgv));

  /* migrate to baserefspec model if necessary */
  g_key_file_set_value (self->origin, "origin", "baserefspec",
                        self->origin_refspec);
  if (!g_key_file_remove_key (self->origin, "origin", "refspec", error))
    {
      if (g_error_matches (*error, G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        g_clear_error (error);
      else
        goto out;
    }

  /* reread spec file --> this will update the current requested_packages */
  if (!parse_refspec (self, cancellable, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

static gboolean
checkout_min_tree_in_tmp (OstreeRepo            *repo,
                          const char            *revision,
                          char                 **out_tmprootfs,
                          int                   *out_tmprootfs_dfd,
                          GCancellable          *cancellable,
                          GError               **error)
{
  gboolean ret = FALSE;
  g_autofree char *tmprootfs = NULL;
  glnx_fd_close int tmprootfs_dfd = -1;
  int repo_dfd = ostree_repo_get_dfd (repo); /* borrowed */

  {
    g_autofree char *template = NULL;
    template = glnx_fdrel_abspath (repo_dfd, "tmp/rpmostree-commit-XXXXXX");

    if (!rpmostree_checkout_only_rpmdb_tempdir (repo, revision, template,
                                                &tmprootfs, &tmprootfs_dfd,
                                                cancellable, error))
      goto out;
  }

  /* also check out sepolicy so that prepare_install() will be able to sort the
   * packages correctly */
  {
    OstreeRepoCheckoutOptions opts = {0,};

    if (!glnx_shutil_mkdir_p_at (tmprootfs_dfd, "usr/etc", 0777,
                                 cancellable, error))
      goto out;

    opts.subpath = "usr/etc/selinux";

    if (!ostree_repo_checkout_tree_at (repo, &opts, tmprootfs_dfd,
                                       "usr/etc/selinux", revision,
                                       cancellable, error))
      goto out;
  }

  if (out_tmprootfs != NULL)
    *out_tmprootfs = g_steal_pointer (&tmprootfs);

  if (out_tmprootfs_dfd != NULL)
    *out_tmprootfs_dfd = glnx_steal_fd (&tmprootfs_dfd);

  ret = TRUE;
out:
  if (tmprootfs_dfd != -1)
    glnx_shutil_rm_rf_at (AT_FDCWD, tmprootfs, cancellable, NULL);
  return ret;
}

static gboolean
checkout_tree_in_tmp (OstreeRepo            *repo,
                      const char            *revision,
                      OstreeRepoDevInoCache *devino_cache,
                      char                 **out_tmprootfs,
                      int                   *out_tmprootfs_dfd,
                      GCancellable          *cancellable,
                      GError               **error)
{
  gboolean ret = FALSE;
  OstreeRepoCheckoutOptions checkout_options = { 0, };

  g_autofree char *tmprootfs = g_strdup ("tmp/rpmostree-commit-XXXXXX");
  glnx_fd_close int tmprootfs_dfd = -1;

  int repo_dfd = ostree_repo_get_dfd (repo); /* borrowed */

  if (!glnx_mkdtempat (repo_dfd, tmprootfs, 00755, error))
    goto out;

  if (!glnx_opendirat (repo_dfd, tmprootfs, FALSE, &tmprootfs_dfd, error))
    goto out;

  /* let's give the user some feedback so they don't think we're blocked */
  rpmostree_output_task_begin ("Checking out tree %.7s", revision);

  /* we actually only need this here because we use "." for path */
  checkout_options.overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
  checkout_options.devino_to_csum_cache = devino_cache;
  if (!ostree_repo_checkout_tree_at (repo, &checkout_options, tmprootfs_dfd,
                                     ".", revision, cancellable, error))
    goto out;

  rpmostree_output_task_end ("done");

  if (out_tmprootfs != NULL)
    *out_tmprootfs = glnx_fdrel_abspath (repo_dfd, tmprootfs);

  if (out_tmprootfs_dfd != NULL)
    *out_tmprootfs_dfd = glnx_steal_fd (&tmprootfs_dfd);

  ret = TRUE;
out:
  if (tmprootfs_dfd != -1)
    glnx_shutil_rm_rf_at (AT_FDCWD, tmprootfs, cancellable, NULL);
  return ret;
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
      HyQuery query = hy_query_create (rsack->sack);
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
                             OstreeRepo               *repo,
                             const char               *base_rev,
                             int                       tmprootfs_dfd,
                             GCancellable             *cancellable,
                             GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) pkgset = hashset_from_strv (self->requested_packages);

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

  if (!find_pkgs_in_rpmdb (tmprootfs_dfd, pkgset, pkg_find_cb,
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
final_assembly (RpmOstreeSysrootUpgrader *self,
                RpmOstreeContext         *ctx,
                OstreeRepo               *repo,
                const char               *base_rev,
                GCancellable             *cancellable,
                GError                  **error)
{
  gboolean ret = FALSE;
  g_autofree char *tmprootfs = NULL;
  glnx_fd_close int tmprootfs_dfd = -1;
  OstreeRepoDevInoCache *devino_cache = ostree_repo_devino_cache_new ();

  /* Now, we create a new tmprootfs containing the *full* tree. Yes, we're
   * wasting an already partially checked out tmprootfs, but some of that stuff
   * is checked out in user mode, plus the dir perms are all made up. */
  if (!checkout_tree_in_tmp (repo, base_rev, devino_cache, &tmprootfs,
                             &tmprootfs_dfd, cancellable, error))
    goto out;

  /* --- Overlay and commit --- */
  if (!rpmostree_context_assemble_commit (ctx, tmprootfs_dfd, devino_cache,
                                          base_rev, &self->new_revision,
                                          cancellable, error))
    goto out;

  ret = TRUE;
out:
  if (devino_cache)
    ostree_repo_devino_cache_unref (devino_cache);
  if (tmprootfs_dfd != -1)
    glnx_shutil_rm_rf_at (AT_FDCWD, tmprootfs, cancellable, NULL);
  return ret;
}

static gboolean
overlay_final_pkgset (RpmOstreeSysrootUpgrader *self,
                      const char               *tmprootfs,
                      int                       tmprootfs_dfd,
                      OstreeRepo               *repo,
                      const char               *base_rev,
                      GCancellable             *cancellable,
                      GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(RpmOstreeContext) ctx = NULL;
  g_autoptr(RpmOstreeTreespec) treespec = NULL;
  g_autoptr(RpmOstreeInstall) install = {0,};
  g_autoptr(GHashTable) pkgset = hashset_from_strv (self->requested_packages);
  glnx_unref_object OstreeRepo *pkgcache_repo = NULL;

  { int n = g_hash_table_size (pkgset);
    GHashTableIter it;
    gpointer itkey;

    g_assert (n > 0);
    g_print ("Need to overlay %d package%s onto tree %.7s:\n",
             n, n > 1 ? "s" : "", base_rev);

    g_hash_table_iter_init (&it, pkgset);
    while (g_hash_table_iter_next (&it, &itkey, NULL))
      g_print ("  %s\n", (char*)itkey);
  }

  ctx = rpmostree_context_new_system (cancellable, error);

  /* point libhif to the yum.repos.d and os-release of the merge_deployment */
  { HifContext *hifctx = rpmostree_context_get_hif (ctx);
    g_autofree char *sysroot_path =
      g_file_get_path (ostree_sysroot_get_path (self->sysroot));
    g_autofree char *merge_deployment_dirpath =
      ostree_sysroot_get_deployment_dirpath (self->sysroot,
                                             self->merge_deployment);
    g_autofree char *merge_deployment_root =
      g_build_filename (sysroot_path, merge_deployment_dirpath, NULL);
    g_autofree char *reposdir = g_build_filename (merge_deployment_root,
                                                  "etc/yum.repos.d", NULL);
    hif_context_set_repo_dir (hifctx, reposdir);
    hif_context_set_source_root (hifctx, merge_deployment_root);
  }

  /* load the sepolicy to use during import */
  {
    glnx_unref_object OstreeSePolicy *sepolicy = NULL;
    if (!rpmostree_prepare_rootfs_get_sepolicy (tmprootfs_dfd, ".", &sepolicy,
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

  if (!rpmostree_context_setup (ctx, tmprootfs, NULL, treespec,
                                cancellable, error))
    goto out;

  if (!get_pkgcache_repo (repo, &pkgcache_repo, cancellable, error))
    goto out;

  rpmostree_context_set_repo (ctx, pkgcache_repo);

  /* --- Downloading metadata --- */
  if (!rpmostree_context_download_metadata (ctx, cancellable, error))
    goto out;

  /* --- Resolving dependencies --- */
  if (!rpmostree_context_prepare_install (ctx, &install, cancellable, error))
    goto out;

  /* we just printed the transaction in prepare_install() -- leave here if it's
   * a dry run */
  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN)
    {
      ret = TRUE;
      goto out;
    }

  /* --- Download as necessary --- */
  if (!rpmostree_context_download (ctx, install, cancellable, error))
    goto out;

  /* --- Import as necessary --- */
  if (!rpmostree_context_import (ctx, install, cancellable, error))
    goto out;

  /* --- Relabel as necessary --- */
  if (!rpmostree_context_relabel (ctx, install, cancellable, error))
    goto out;

  /* --- Overlay packages on base layer --- */
  if (!final_assembly (self, ctx, repo, base_rev, cancellable, error))
    goto out;

  /* Send the final commit to the parent repo */
  {
    GFile *repo_path = ostree_repo_get_path (pkgcache_repo);
    g_autofree char *uri =
      g_strdup_printf ("file://%s", gs_file_get_path_cached (repo_path));

    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&builder, "{s@v}", "refs", g_variant_new_variant
      (g_variant_new_strv ((const char * const*)&self->new_revision, 1)));

    if (!ostree_repo_pull_with_options (repo, uri,
                                        g_variant_builder_end (&builder),
                                        NULL, cancellable, error))
      goto out;
  }

  ret = TRUE;
out:
  return ret;
}

static gboolean
overlay_packages (RpmOstreeSysrootUpgrader *self,
                  GCancellable             *cancellable,
                  GError                  **error)
{
  gboolean ret = FALSE;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autofree char *tmprootfs = NULL;
  glnx_fd_close int tmprootfs_dfd = -1;
  g_autofree char *base_rev = NULL;

  if (!ostree_sysroot_get_repo (self->sysroot, &repo, cancellable, error))
    goto out;

  /* determine the base commit on which to layer stuff */

  /* the only case in which new_revision isn't a "base" commit is if we're
   * redeploying and there's already stuff layered down, in which case, it's the
   * parent commit */
  if ((self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_REDEPLOY) &&
      (g_strv_length (self->requested_packages) > 0))
    {
      if (!commit_get_parent_csum (repo, self->new_revision, &base_rev, error))
        goto out;
      g_assert (base_rev);
    }
  else
    base_rev = g_strdup (self->new_revision);

  /* create a tmprootfs in which we initially only check out the bare minimum to
   * make libhif happy. this allows us to provide more immediate feedback to the
   * user if e.g. resolving/downloading/etc... goes wrong without making them
   * wait for a full tree checkout first. */
  if (!checkout_min_tree_in_tmp (repo, base_rev, &tmprootfs, &tmprootfs_dfd,
                                 cancellable, error))
    goto out;

  /* check if there are any items in requested_packages or pkgs_to_add that are
   * already installed in base_rev */
  if (!finalize_requested_packages (self, repo, base_rev, tmprootfs_dfd,
                                    cancellable, error))
    goto out;

  /* trivial case: no packages to overlay */
  if (g_strv_length (self->requested_packages) == 0)
    {
      /* XXX: check if there's a function for this */
      g_free (self->new_revision);
      self->new_revision = g_steal_pointer (&base_rev);
    }
  else
    {
      if (!overlay_final_pkgset (self, tmprootfs, tmprootfs_dfd, repo, base_rev,
                                 cancellable, error))
        goto out;
    }

  ret = TRUE;
out:
  if (tmprootfs_dfd != -1)
    glnx_shutil_rm_rf_at (AT_FDCWD, tmprootfs, cancellable, NULL);
  return ret;
}

/* For each deployment (including the new one yet to be written), if they are
 * layered deployments, then create a ref pointing to their bases. This is
 * mostly to work around ostree's auto-ref cleanup. Otherwise we might get into
 * a situation where after the origin ref is updated, we lose our parent, which
 * means that users can no longer add/delete packages on that deployment. (They
 * can always just re-pull it, but let's try to be nice). */
static gboolean
generate_baselayer_refs (RpmOstreeSysrootUpgrader *self,
                         OstreeDeployment         *new_deployment,
                         GCancellable             *cancellable,
                         GError                  **error)
{
  gboolean ret = FALSE;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autoptr(GHashTable) refs = NULL;
  g_autoptr(GHashTable) bases =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (!ostree_sysroot_get_repo (self->sysroot, &repo, cancellable, error))
    goto out;

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
      ostree_sysroot_get_deployments (self->sysroot);

    /* existing deployments */
    for (; i < deployments->len; i++)
      {
        OstreeDeployment *deployment = deployments->pdata[i];
        GKeyFile *origin = ostree_deployment_get_origin (deployment);
        g_auto(GStrv) packages = NULL;

        if (!_rpmostree_util_parse_origin (origin, NULL, &packages, error))
          goto out;

        if (packages && g_strv_length (packages) > 0)
          {
            const char *csum = ostree_deployment_get_csum (deployment);
            g_autofree char *base_rev = NULL;

            if (!commit_get_parent_csum (repo, csum, &base_rev, error))
              goto out;
            g_assert (base_rev);

            g_hash_table_add (bases, g_steal_pointer (&base_rev));
          }
      }

    /* our new deployment, in case we're called before it's added */
    if (g_strv_length (self->requested_packages) > 0)
      {
        g_autofree char *base_rev = NULL;

        if (!commit_get_parent_csum (repo, self->new_revision, &base_rev, error))
          goto out;
        g_assert (base_rev);

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

  /* make sure we have a known target to deploy */
  g_assert (self->new_revision);

  /* any packages requested for overlay? */
  if ((g_strv_length (self->requested_packages) > 0) ||
      (g_hash_table_size (self->packages_to_add) > 0) ||
      (g_hash_table_size (self->packages_to_delete) > 0))
    if (!overlay_packages (self, cancellable, error))
      goto out;

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN)
    {
      ret = TRUE;
      goto out;
    }

  if (!ostree_sysroot_deploy_tree (self->sysroot, self->osname,
                                   self->new_revision,
                                   self->origin,
                                   self->merge_deployment,
                                   NULL,
                                   &new_deployment,
                                   cancellable, error))
    goto out;

  if (!generate_baselayer_refs (self, new_deployment, cancellable, error))
    goto out;

  if (!ostree_sysroot_simple_write_deployment (self->sysroot, self->osname,
                                               new_deployment,
                                               self->merge_deployment,
                                               0,
                                               cancellable, error))
    goto out;

  /* regenerate the baselayer refs in case we just kicked out an ancient layered
   * deployment whose base layer is not needed anymore */
  if (!generate_baselayer_refs (self, new_deployment, cancellable, error))
    goto out;

  /* and shake it loose */
  if (!ostree_sysroot_cleanup (self->sysroot, cancellable, error))
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
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_REDEPLOY,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_REDEPLOY",
          "redeploy" },
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
