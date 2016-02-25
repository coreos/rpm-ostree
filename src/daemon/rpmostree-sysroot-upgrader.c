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

  if (!_rpmostree_util_parse_origin (self->origin, &self->origin_refspec, &self->requested_packages, error))
    goto out;

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

  self->origin = ostree_deployment_get_origin (self->merge_deployment);
  if (!self->origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin known for deployment %s.%d",
                   ostree_deployment_get_csum (self->merge_deployment),
                   ostree_deployment_get_deployserial (self->merge_deployment));
      goto out;
    }
  g_key_file_ref (self->origin);

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
  g_free (self->override_csum);

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

gboolean
rpmostree_sysroot_upgrader_set_origin_rebase (RpmOstreeSysrootUpgrader *self, const char *new_refspec, GError **error)
{
  g_free (self->origin_refspec);
  self->origin_refspec = g_strdup (new_refspec);
  return TRUE;
}

void
rpmostree_sysroot_upgrader_set_origin_override (RpmOstreeSysrootUpgrader *self, const char *override_commit)
{
  if (override_commit != NULL)
    g_key_file_set_string (self->origin, "origin", "override-commit", override_commit);
  else
    g_key_file_remove_key (self->origin, "origin", "override_commit", NULL);
}

void
rpmostree_sysroot_upgrader_set_origin_baseref_local (RpmOstreeSysrootUpgrader *self, const char *local_commit)
{
  self->new_revision = g_strdup (local_commit);
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

/*
 * Like ostree_sysroot_upgrader_pull(), but will modify to include
 * layered packages.
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

  if (g_strcmp0 (from_revision, self->new_revision) == 0)
    {
      *out_changed = FALSE;
    }
  else
    {
      gboolean allow_older = (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER) > 0;

      *out_changed = TRUE;

      if (from_revision && !allow_older)
        {
          if (!ostree_sysroot_upgrader_check_timestamps (repo, from_revision,
                                                         self->new_revision,
                                                         error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * rpmostree_sysroot_upgrader_deploy:
 * @self: Self
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write the new deployment to disk, perform a configuration merge
 * with /etc, and update the bootloader configuration.
 */
gboolean
rpmostree_sysroot_upgrader_deploy (RpmOstreeSysrootUpgrader  *self,
                                   GCancellable           *cancellable,
                                   GError                **error)
{
  gboolean ret = FALSE;
  glnx_unref_object OstreeDeployment *new_deployment = NULL;

  if (!ostree_sysroot_deploy_tree (self->sysroot, self->osname,
                                   self->new_revision,
                                   self->origin,
                                   self->merge_deployment,
                                   NULL,
                                   &new_deployment,
                                   cancellable, error))
    goto out;

  if (!ostree_sysroot_simple_write_deployment (self->sysroot, self->osname,
                                               new_deployment,
                                               self->merge_deployment,
                                               0,
                                               cancellable, error))
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
          "allow-older" }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("RpmOstreeSysrootUpgraderFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
