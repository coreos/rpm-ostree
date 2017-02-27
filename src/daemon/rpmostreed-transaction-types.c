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
#include "ostree.h"

#include <libglnx.h>

#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-sysroot.h"
#include "rpmostree-sysroot-upgrader.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostreed-utils.h"

static gboolean
change_origin_refspec (OstreeSysroot *sysroot,
                       RpmOstreeOrigin *origin,
                       const gchar *refspec,
                       GCancellable *cancellable,
                       gchar **out_old_refspec,
                       gchar **out_new_refspec,
                       GError **error)
{
  gboolean ret = FALSE;
  g_autofree gchar *new_refspec = NULL;
  g_autofree gchar *current_refspec =
    g_strdup (rpmostree_origin_get_refspec (origin));

  if (!rpmostreed_refspec_parse_partial (refspec,
                                         current_refspec,
                                         &new_refspec,
                                         error))
    goto out;

  if (strcmp (current_refspec, new_refspec) == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Old and new refs are equal: %s", new_refspec);
      goto out;
    }


  if (!rpmostree_origin_set_rebase (origin, new_refspec, error))
    goto out;

  if (out_new_refspec != NULL)
    *out_new_refspec = g_steal_pointer (&new_refspec);

  if (out_old_refspec != NULL)
    *out_old_refspec = g_strdup (current_refspec);

  ret = TRUE;

out:
  return ret;
}

static gboolean
apply_revision_override (RpmostreedTransaction    *transaction,
                         OstreeRepo               *repo,
                         OstreeAsyncProgress      *progress,
                         RpmOstreeOrigin          *origin,
                         const char               *revision,
                         GCancellable             *cancellable,
                         GError                  **error)
{
  g_autofree char *checksum = NULL;
  g_autofree char *version = NULL;

  if (!rpmostreed_parse_revision (revision,
                                  &checksum,
                                  &version,
                                  error))
    return FALSE;

  if (version != NULL)
    {
      rpmostreed_transaction_emit_message_printf (transaction,
                                                  "Resolving version '%s'",
                                                  version);

      if (!rpmostreed_repo_lookup_version (repo, rpmostree_origin_get_refspec (origin),
                                           version, progress,
                                           cancellable, &checksum, error))
        return FALSE;
    }
  else
    {
      g_assert (checksum != NULL);

      rpmostreed_transaction_emit_message_printf (transaction,
                                                  "Validating checksum '%s'",
                                                  checksum);

      if (!rpmostreed_repo_lookup_checksum (repo, rpmostree_origin_get_refspec (origin),
                                            checksum, progress, cancellable, error))
        return FALSE;
    }

  rpmostree_origin_set_override_commit (origin, checksum, version);

  return TRUE;
}

/* ============================= Package Diff  ============================= */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  char *refspec;
  char *revision;
} PackageDiffTransaction;

typedef RpmostreedTransactionClass PackageDiffTransactionClass;

GType package_diff_transaction_get_type (void);

G_DEFINE_TYPE (PackageDiffTransaction,
               package_diff_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
package_diff_transaction_finalize (GObject *object)
{
  PackageDiffTransaction *self;

  self = (PackageDiffTransaction *) object;
  g_free (self->osname);
  g_free (self->refspec);
  g_free (self->revision);

  G_OBJECT_CLASS (package_diff_transaction_parent_class)->finalize (object);
}

static gboolean
package_diff_transaction_execute (RpmostreedTransaction *transaction,
                                  GCancellable *cancellable,
                                  GError **error)
{
  PackageDiffTransaction *self;
  OstreeSysroot *sysroot;

  glnx_unref_object RpmOstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  glnx_unref_object OstreeDeployment *merge_deployment = NULL;
  g_autoptr(RpmOstreeOrigin) origin = NULL;

  RpmOstreeSysrootUpgraderFlags upgrader_flags = 0;
  gboolean upgrading = FALSE;
  gboolean changed = FALSE;
  gboolean ret = FALSE;

  self = (PackageDiffTransaction *) transaction;

  if (self->revision != NULL || self->refspec != NULL)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);
  upgrader = rpmostree_sysroot_upgrader_new (sysroot,
                                             self->osname,
                                             upgrader_flags,
                                             cancellable,
                                             error);
  if (upgrader == NULL)
    goto out;

  origin = rpmostree_sysroot_upgrader_dup_origin (upgrader);

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  merge_deployment = ostree_sysroot_get_merge_deployment (sysroot, self->osname);

  /* Determine if we're upgrading before we set the refspec. */
  upgrading = (self->refspec == NULL && self->revision == NULL);

  if (self->refspec != NULL)
    {
      if (!change_origin_refspec (sysroot, origin, self->refspec,
                                  cancellable, NULL, NULL, error))
        goto out;
    }

  progress = ostree_async_progress_new ();
  rpmostreed_transaction_connect_download_progress (transaction, progress);
  rpmostreed_transaction_connect_signature_progress (transaction, repo);

  if (self->revision != NULL)
    {
      if (!apply_revision_override (transaction, repo, progress, origin,
                                    self->revision, cancellable, error))
        goto out;
    }
  else if (upgrading)
    {
      rpmostree_origin_set_override_commit (origin, NULL, NULL);
    }

  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);

  rpmostreed_transaction_emit_message_printf (transaction,
                                              "Updating from: %s",
                                              self->refspec);

  if (!rpmostree_sysroot_upgrader_pull (upgrader,
                                        "/usr/share/rpm",
                                        0,
                                        progress,
                                        &changed,
                                        cancellable,
                                        error))
    goto out;

  rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

  if (!changed)
    {
      if (upgrading)
        rpmostreed_transaction_emit_message_printf (transaction,
                                                    "No upgrade available.");
      else
        rpmostreed_transaction_emit_message_printf (transaction,
                                                    "No change.");
    }

  ret = TRUE;
out:
  return ret;
}

static void
package_diff_transaction_class_init (PackageDiffTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = package_diff_transaction_finalize;

  class->execute = package_diff_transaction_execute;
}

static void
package_diff_transaction_init (PackageDiffTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_package_diff (GDBusMethodInvocation *invocation,
                                         OstreeSysroot *sysroot,
                                         const char *osname,
                                         const char *refspec,
                                         const char *revision,
                                         GCancellable *cancellable,
                                         GError **error)
{
  PackageDiffTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);

  self = g_initable_new (package_diff_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->refspec = g_strdup (refspec);
      self->revision = g_strdup (revision);
    }

  return (RpmostreedTransaction *) self;
}

/* =============================== Rollback =============================== */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  gboolean reboot;
} RollbackTransaction;

typedef RpmostreedTransactionClass RollbackTransactionClass;

GType rollback_transaction_get_type (void);

G_DEFINE_TYPE (RollbackTransaction,
               rollback_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
rollback_transaction_finalize (GObject *object)
{
  RollbackTransaction *self;

  self = (RollbackTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (rollback_transaction_parent_class)->finalize (object);
}

static gboolean
rollback_transaction_execute (RpmostreedTransaction *transaction,
                              GCancellable *cancellable,
                              GError **error)
{
  RollbackTransaction *self;
  OstreeSysroot *sysroot;
  OstreeDeployment *deployment;
  g_autoptr(GPtrArray) old_deployments = NULL;
  g_autoptr(GPtrArray) new_deployments = NULL;

  gint rollback_index;
  guint i;
  gboolean ret = FALSE;

  self = (RollbackTransaction *) transaction;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);

  rollback_index = rpmostreed_rollback_deployment_index (self->osname, sysroot, error);
  if (rollback_index < 0)
    goto out;

  old_deployments = ostree_sysroot_get_deployments (sysroot);
  new_deployments = g_ptr_array_new_with_free_func (g_object_unref);

  /* build out the reordered array */

  deployment = old_deployments->pdata[rollback_index];
  g_ptr_array_add (new_deployments, g_object_ref (deployment));

  rpmostreed_transaction_emit_message_printf (transaction,
                                              "Moving '%s.%d' to be first deployment",
                                              ostree_deployment_get_csum (deployment),
                                              ostree_deployment_get_deployserial (deployment));

  for (i = 0; i < old_deployments->len; i++)
    {
      if (i == rollback_index)
        continue;

      deployment = old_deployments->pdata[i];
      g_ptr_array_add (new_deployments, g_object_ref (deployment));
    }

  /* if default changed write it */
  if (old_deployments->pdata[0] != new_deployments->pdata[0])
    {
      if (!ostree_sysroot_write_deployments (sysroot,
					     new_deployments,
					     cancellable,
					     error))
        goto out;
    }

  if (self->reboot)
    rpmostreed_reboot (cancellable, error);

  ret = TRUE;

out:
  return ret;
}

static void
rollback_transaction_class_init (RollbackTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = rollback_transaction_finalize;

  class->execute = rollback_transaction_execute;
}

static void
rollback_transaction_init (RollbackTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_rollback (GDBusMethodInvocation *invocation,
                                     OstreeSysroot *sysroot,
                                     const char *osname,
                                     gboolean reboot,
                                     GCancellable *cancellable,
                                     GError **error)
{
  RollbackTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);

  self = g_initable_new (rollback_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->reboot = reboot;
    }

  return (RpmostreedTransaction *) self;
}

/* ============================ Clear Rollback ============================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  gboolean reboot;
} ClearRollbackTransaction;

typedef RpmostreedTransactionClass ClearRollbackTransactionClass;

GType clear_rollback_transaction_get_type (void);

G_DEFINE_TYPE (ClearRollbackTransaction,
               clear_rollback_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
clear_rollback_transaction_finalize (GObject *object)
{
  ClearRollbackTransaction *self;

  self = (ClearRollbackTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (clear_rollback_transaction_parent_class)->finalize (object);
}

static gboolean
clear_rollback_transaction_execute (RpmostreedTransaction *transaction,
                                    GCancellable *cancellable,
                                    GError **error)
{
  ClearRollbackTransaction *self;
  OstreeSysroot *sysroot;

  g_autoptr(GPtrArray) deployments = NULL;
  gint rollback_index;
  gboolean ret = FALSE;

  self = (ClearRollbackTransaction *) transaction;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);

  rollback_index = rpmostreed_rollback_deployment_index (self->osname, sysroot, error);
  if (rollback_index < 0)
    goto out;

  deployments = ostree_sysroot_get_deployments (sysroot);

  if (deployments->pdata[rollback_index] == ostree_sysroot_get_booted_deployment (sysroot))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Cannot undeploy currently booted deployment %i",
                   rollback_index);
      goto out;
    }

  g_ptr_array_remove_index (deployments, rollback_index);

  if (!ostree_sysroot_write_deployments (sysroot,
					 deployments,
					 cancellable,
					 error))
    goto out;

  if (self->reboot)
    rpmostreed_reboot (cancellable, error);

  ret = TRUE;

out:
  return ret;
}

static void
clear_rollback_transaction_class_init (ClearRollbackTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = clear_rollback_transaction_finalize;

  class->execute = clear_rollback_transaction_execute;
}

static void
clear_rollback_transaction_init (ClearRollbackTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_clear_rollback (GDBusMethodInvocation *invocation,
                                           OstreeSysroot *sysroot,
                                           const char *osname,
                                           gboolean reboot,
                                           GCancellable *cancellable,
                                           GError **error)
{
  ClearRollbackTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);

  self = g_initable_new (clear_rollback_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->reboot = reboot;
    }

  return (RpmostreedTransaction *) self;
}

/* ================================ Upgrade/Deploy/Rebase ================================ */

typedef struct {
  RpmostreedTransaction parent;
  RpmOstreeTransactionDeployFlags flags;
  char *osname;
  char *refspec; /* NULL for non-rebases */
  char *revision; /* NULL for upgrade */
  char **packages_added;
  char **packages_removed;
} DeployTransaction;

typedef RpmostreedTransactionClass DeployTransactionClass;

GType deploy_transaction_get_type (void);

G_DEFINE_TYPE (DeployTransaction,
               deploy_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
deploy_transaction_finalize (GObject *object)
{
  DeployTransaction *self;

  self = (DeployTransaction *) object;
  g_free (self->osname);
  g_free (self->refspec);
  g_free (self->revision);
  g_strfreev (self->packages_added);
  g_strfreev (self->packages_removed);

  G_OBJECT_CLASS (deploy_transaction_parent_class)->finalize (object);
}

static gboolean
deploy_transaction_execute (RpmostreedTransaction *transaction,
                            GCancellable *cancellable,
                            GError **error)
{
  DeployTransaction *self;
  OstreeSysroot *sysroot;
  glnx_unref_object RpmOstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  g_autofree gchar *new_refspec = NULL;
  g_autofree gchar *old_refspec = NULL;
  RpmOstreeSysrootUpgraderFlags upgrader_flags = 0;
  gboolean changed = FALSE;
  gboolean ret = FALSE;
  g_autoptr(RpmOstreeOrigin) origin = NULL;

  self = (DeployTransaction *) transaction;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);

  if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER;
  if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DRY_RUN)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN;
  if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NOSCRIPTS)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_NOSCRIPTS;

  if (self->refspec)
    {
      /* When rebasing, we should be able to switch to a different tree even if
       * the current origin is unconfigured */
      upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED;
    }

  upgrader = rpmostree_sysroot_upgrader_new (sysroot, self->osname,
                                             upgrader_flags,
                                             cancellable, error);
  if (upgrader == NULL)
    goto out;

  origin = rpmostree_sysroot_upgrader_dup_origin (upgrader);

  if (self->refspec)
    {
      if (!change_origin_refspec (sysroot, origin, self->refspec, cancellable,
                                  &old_refspec, &new_refspec, error))
        goto out;
    }

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  progress = ostree_async_progress_new ();
  rpmostreed_transaction_connect_download_progress (transaction, progress);
  rpmostreed_transaction_connect_signature_progress (transaction, repo);

  if (self->revision)
    {
      if (!apply_revision_override (transaction, repo, progress, origin,
                                    self->revision, cancellable, error))
        goto out;
    }
  else
    {
      rpmostree_origin_set_override_commit (origin, NULL, NULL);
    }

  if (self->packages_removed)
    {
      if (!rpmostree_origin_delete_packages (origin, self->packages_removed,
                                             cancellable, error))
        goto out;

      /* in reality, there may not be any new layer required (if e.g. we're
       * removing a duplicate provides), though the origin has changed so we
       * need to create a new deployment */
      changed = TRUE;
    }

  if (self->packages_added)
    {
      if (!rpmostree_origin_add_packages (origin, self->packages_added,
                                          cancellable, error))
        goto out;

      changed = TRUE;
    }

  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);

  /* Mainly for the `install` command */
  if (!(self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_PULL_BASE))
    {
      gboolean base_changed;

      if (!rpmostree_sysroot_upgrader_pull (upgrader, NULL, 0, progress,
                                            &base_changed, cancellable, error))
        goto out;

      changed = changed || base_changed;
    }

  rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

  /* TODO - better logic for "changed" based on deployments */
  if (changed || self->refspec)
    {
      if (!rpmostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
        goto out;

      /* Are we rebasing?  May want to delete the previous ref */
      if (self->refspec && !(self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_SKIP_PURGE))
        {
          g_autofree char *remote = NULL;
          g_autofree char *ref = NULL;

          /* The actual rebase has already succeeded, so ignore errors. */
          if (ostree_parse_refspec (old_refspec, &remote, &ref, NULL))
            {
              /* Note: In some cases the source origin ref may not actually
               * exist; say the admin did a cleanup, or the OS expects post-
               * install configuration like subscription-manager. */
              (void) ostree_repo_set_ref_immediate (repo, remote, ref, NULL,
                                                    cancellable, NULL);
            }
        }

      if (self->flags & RPMOSTREE_TRANSACTION_DEPLOY_FLAG_REBOOT)
        rpmostreed_reboot (cancellable, error);
    }
  else
    {
      if (!self->revision)
        rpmostreed_transaction_emit_message_printf (transaction, "No upgrade available.");
      else
        rpmostreed_transaction_emit_message_printf (transaction, "No change.");
    }

  ret = TRUE;
out:
  return ret;
}

static void
deploy_transaction_class_init (DeployTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = deploy_transaction_finalize;

  class->execute = deploy_transaction_execute;
}

static void
deploy_transaction_init (DeployTransaction *self)
{
}

static char **
strdupv_canonicalize (const char *const *strv)
{
  if (strv && *strv)
    return g_strdupv ((char**)strv);
  return NULL;
}

RpmostreedTransaction *
rpmostreed_transaction_new_deploy (GDBusMethodInvocation *invocation,
                                   OstreeSysroot *sysroot,
                                   RpmOstreeTransactionDeployFlags flags,
                                   const char *osname,
                                   const char *refspec,
                                   const char *revision,
                                   const char *const *packages_added,
                                   const char *const *packages_removed,
                                   GCancellable *cancellable,
                                   GError **error)
{
  DeployTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);

  self = g_initable_new (deploy_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->flags = flags;
      self->refspec = g_strdup (refspec);
      self->revision = g_strdup (revision);
      self->packages_added = strdupv_canonicalize (packages_added);
      self->packages_removed = strdupv_canonicalize (packages_removed);
    }

  return (RpmostreedTransaction *) self;
}

/* ================================ InitramfsState ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  gboolean regenerate;
  char **args;
  gboolean reboot;
} InitramfsStateTransaction;

typedef RpmostreedTransactionClass InitramfsStateTransactionClass;

GType initramfs_state_transaction_get_type (void);

G_DEFINE_TYPE (InitramfsStateTransaction,
               initramfs_state_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
initramfs_state_transaction_finalize (GObject *object)
{
  InitramfsStateTransaction *self;

  self = (InitramfsStateTransaction *) object;
  g_free (self->osname);
  g_strfreev (self->args);

  G_OBJECT_CLASS (initramfs_state_transaction_parent_class)->finalize (object);
}

static gboolean
initramfs_state_transaction_execute (RpmostreedTransaction *transaction,
                            GCancellable *cancellable,
                            GError **error)
{
  InitramfsStateTransaction *self;
  OstreeSysroot *sysroot;
  glnx_unref_object RpmOstreeSysrootUpgrader *upgrader = NULL;
  g_autoptr(RpmOstreeOrigin) origin = NULL;
  const char *const* current_initramfs_args = NULL;
  gboolean current_regenerate;

  self = (InitramfsStateTransaction *) transaction;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);

  upgrader = rpmostree_sysroot_upgrader_new (sysroot, self->osname, 0,
                                             cancellable, error);
  if (upgrader == NULL)
    return FALSE;

  origin = rpmostree_sysroot_upgrader_dup_origin (upgrader);
  current_regenerate = rpmostree_origin_get_regenerate_initramfs (origin);
  current_initramfs_args = rpmostree_origin_get_initramfs_args (origin);

  /* We don't deep-compare the args right now, we assume if you were using them
   * you want to rerun. This can be important if you edited a config file, which
   * we can't really track without actually regenerating anyways.
   */
  if (current_regenerate == self->regenerate
      && (current_initramfs_args == NULL || !*current_initramfs_args)
      && (self->args == NULL || !*self->args))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "initramfs regeneration state is already %s",
                   current_regenerate ? "enabled" : "disabled");
      return FALSE;
    }

  rpmostree_origin_set_regenerate_initramfs (origin, self->regenerate, self->args);
  rpmostree_sysroot_upgrader_set_origin (upgrader, origin);

  if (!rpmostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
    return FALSE;

  if (self->reboot)
    rpmostreed_reboot (cancellable, error);

  return TRUE;
}

static void
initramfs_state_transaction_class_init (InitramfsStateTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = initramfs_state_transaction_finalize;

  class->execute = initramfs_state_transaction_execute;
}

static void
initramfs_state_transaction_init (InitramfsStateTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_initramfs_state (GDBusMethodInvocation *invocation,
                                            OstreeSysroot *sysroot,
                                            const char *osname,
                                            gboolean regenerate,
                                            char **args,
                                            gboolean reboot,
                                            GCancellable *cancellable,
                                            GError **error)
{
  InitramfsStateTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);

  self = g_initable_new (initramfs_state_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->regenerate = regenerate;
      self->args = g_strdupv (args);
      self->reboot = reboot;
    }

  return (RpmostreedTransaction *) self;
}

/* ================================ Cleanup ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  RpmOstreeTransactionCleanupFlags flags;
} CleanupTransaction;

typedef RpmostreedTransactionClass CleanupTransactionClass;

GType cleanup_transaction_get_type (void);

G_DEFINE_TYPE (CleanupTransaction,
               cleanup_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
cleanup_transaction_finalize (GObject *object)
{
  CleanupTransaction *self;

  self = (CleanupTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (cleanup_transaction_parent_class)->finalize (object);
}

static gboolean
remove_directory_content_if_exists (int dfd,
                                    const char *path,
                                    GCancellable *cancellable,
                                    GError **error)
{
  glnx_fd_close int fd = -1;
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  fd = glnx_opendirat_with_errno (dfd, path, TRUE);
  if (fd < 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }
  else
    {
      if (!glnx_dirfd_iterator_init_take_fd (fd, &dfd_iter, error))
        return FALSE;
      fd = -1;

      while (TRUE)
        {
          struct dirent *dent = NULL;

          if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
            return FALSE;
          if (dent == NULL)
            break;

          if (!glnx_shutil_rm_rf_at (dfd_iter.fd, dent->d_name, cancellable, error))
            return FALSE;
        }
    }
  return TRUE;
}

/* This is a bit like ostree_sysroot_simple_write_deployment() */
static GPtrArray *
get_filtered_deployments (OstreeSysroot     *sysroot,
                          const char        *osname,
                          gboolean cleanup_pending,
                          gboolean cleanup_rollback)
{
  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  g_autoptr(GPtrArray) new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  OstreeDeployment *booted_deployment = NULL;
  gboolean found_booted = FALSE;

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];

      /* Is this deployment booted?  If so, note we're past the booted,
       * and ensure it's added. */
      if (booted_deployment != NULL &&
          ostree_deployment_equal (deployment, booted_deployment))
        {
          found_booted = TRUE;
          g_ptr_array_add (new_deployments, g_object_ref (deployment));
          continue;
        }

      /* Is this deployment for a different osname?  Keep it. */
      if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
        {
          g_ptr_array_add (new_deployments, g_object_ref (deployment));
          continue;
        }

      /* Now, we may skip this deployment, i.e. GC it. */
      if (!found_booted && cleanup_pending)
        continue;

      if (found_booted && cleanup_rollback)
        continue;

      /* Otherwise, add it */
      g_ptr_array_add (new_deployments, g_object_ref (deployment));
    }

  if (new_deployments->len == deployments->len)
    return NULL;
  return g_steal_pointer (&new_deployments);
}

static gboolean
cleanup_transaction_execute (RpmostreedTransaction *transaction,
                             GCancellable *cancellable,
                             GError **error)
{
  CleanupTransaction *self = (CleanupTransaction *) transaction;
  OstreeSysroot *sysroot;
  glnx_unref_object OstreeRepo *repo = NULL;
  const gboolean cleanup_pending = (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_PENDING_DEPLOY) > 0;
  const gboolean cleanup_rollback = (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_ROLLBACK_DEPLOY) > 0;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;

  if (cleanup_pending || cleanup_rollback)
    {
      g_autoptr(GPtrArray) new_deployments = get_filtered_deployments (sysroot, self->osname,
                                                                       cleanup_pending,
                                                                       cleanup_rollback);
      if (new_deployments)
        {
          /* TODO - expose the skip cleanup flag in libostree, use it here */
          if (!ostree_sysroot_write_deployments (sysroot, new_deployments, cancellable, error))
            return FALSE;
          /* And ensure we fall through to base cleanup */
          self->flags |= RPMOSTREE_TRANSACTION_CLEANUP_BASE;
        }
      else
        {
          g_print ("Deployments unchanged.\n");
        }
    }
  if (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_BASE)
    {
      if (!rpmostree_sysroot_upgrader_cleanup (sysroot, repo, cancellable, error))
        return FALSE;
    }
  if (self->flags & RPMOSTREE_TRANSACTION_CLEANUP_REPOMD)
    {
      if (!remove_directory_content_if_exists (AT_FDCWD, RPMOSTREE_CORE_CACHEDIR, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static void
cleanup_transaction_class_init (CleanupTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = cleanup_transaction_finalize;

  class->execute = cleanup_transaction_execute;
}

static void
cleanup_transaction_init (CleanupTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_cleanup (GDBusMethodInvocation *invocation,
                                    OstreeSysroot         *sysroot,
                                    const char            *osname,
                                    RpmOstreeTransactionCleanupFlags flags,
                                    GCancellable          *cancellable,
                                    GError               **error)
{
  CleanupTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);

  self = g_initable_new (cleanup_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->flags = flags;
    }

  return (RpmostreedTransaction *) self;

}
