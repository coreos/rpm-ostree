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
#include "rpmostreed-utils.h"

static gboolean
change_upgrader_refspec (OstreeSysroot *sysroot,
                         RpmOstreeSysrootUpgrader *upgrader,
                         const gchar *refspec,
                         GCancellable *cancellable,
                         gchar **out_old_refspec,
                         gchar **out_new_refspec,
                         GError **error)
{
  gboolean ret = FALSE;
  g_autofree gchar *new_refspec = NULL;
  g_autofree gchar *current_refspec = g_strdup (rpmostree_sysroot_upgrader_get_refspec (upgrader));

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


  if (!rpmostree_sysroot_upgrader_set_origin_rebase (upgrader, new_refspec, error))
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
                         RpmOstreeSysrootUpgrader *upgrader,
                         const char               *revision,
                         GCancellable             *cancellable,
                         GError                  **error)
{
  RpmOstreeOrigin *origin = rpmostree_sysroot_upgrader_get_origin (upgrader);
  g_autoptr(GKeyFile) new_origin = NULL;
  g_autofree char *checksum = NULL;
  g_autofree char *version = NULL;

  new_origin = rpmostree_origin_dup_keyfile (origin);

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

  g_key_file_set_string (new_origin, "origin", "override-commit", checksum);

  if (version != NULL)
    {
      g_autofree char *comment = NULL;

      /* Add a comment with the version, to be nice. */
      comment = g_strdup_printf ("Version %s [%.10s]", version, checksum);
      g_key_file_set_comment (new_origin, "origin", "override-commit", comment, NULL);
    }

  if (!rpmostree_sysroot_upgrader_set_origin (upgrader, new_origin, cancellable, error))
    return FALSE;

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
  g_autofree gchar *origin_description = NULL;

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

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  merge_deployment = ostree_sysroot_get_merge_deployment (sysroot, self->osname);

  /* Determine if we're upgrading before we set the refspec. */
  upgrading = (self->refspec == NULL && self->revision == NULL);

  if (self->refspec != NULL)
    {
      if (!change_upgrader_refspec (sysroot, upgrader,
				    self->refspec, cancellable,
				    NULL, NULL, error))
	goto out;
    }
  else
    {
      self->refspec = g_strdup (rpmostree_sysroot_upgrader_get_refspec (upgrader));
      if (!self->refspec)
	{
	  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			       "Booted deployment has no origin");
	  goto out;
	}
    }

  progress = ostree_async_progress_new ();
  rpmostreed_transaction_connect_download_progress (transaction, progress);
  rpmostreed_transaction_connect_signature_progress (transaction, repo);

  if (self->revision != NULL)
    {
      g_autofree char *checksum = NULL;
      g_autofree char *version = NULL;

      if (!rpmostreed_parse_revision (self->revision,
                                      &checksum,
                                      &version,
                                      error))
        goto out;

      if (version != NULL)
        {
          rpmostreed_transaction_emit_message_printf (transaction,
                                                      "Resolving version '%s'",
                                                      version);

          if (!rpmostreed_repo_lookup_version (repo,
                                               self->refspec,
                                               version,
                                               progress,
                                               cancellable,
                                               &checksum,
                                               error))
            goto out;
        }

      rpmostree_sysroot_upgrader_set_origin_override (upgrader, checksum);
    }
  else if (upgrading)
    {
      rpmostree_sysroot_upgrader_set_origin_override (upgrader, NULL);
    }

  origin_description = rpmostree_sysroot_upgrader_get_origin_description (upgrader);
  if (origin_description != NULL)
    rpmostreed_transaction_emit_message_printf (transaction,
                                                "Updating from: %s",
                                                origin_description);

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

/* ================================ Rebase ================================ */

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  char *refspec;
  char *revision;
  gboolean skip_purge;
  gboolean reboot;
} RebaseTransaction;

typedef RpmostreedTransactionClass RebaseTransactionClass;

GType rebase_transaction_get_type (void);

G_DEFINE_TYPE (RebaseTransaction,
               rebase_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
rebase_transaction_finalize (GObject *object)
{
  RebaseTransaction *self;

  self = (RebaseTransaction *) object;
  g_free (self->osname);
  g_free (self->refspec);

  G_OBJECT_CLASS (rebase_transaction_parent_class)->finalize (object);
}

static gboolean
rebase_transaction_execute (RpmostreedTransaction *transaction,
                            GCancellable *cancellable,
                            GError **error)
{
  RebaseTransaction *self;
  OstreeSysroot *sysroot;

  glnx_unref_object RpmOstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;

  g_autofree gchar *new_refspec = NULL;
  g_autofree gchar *old_refspec = NULL;

  gboolean changed = FALSE;
  gboolean ret = FALSE;

  RpmOstreeSysrootUpgraderFlags flags = 0;

  self = (RebaseTransaction *) transaction;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);

  /* Always allow older; there's not going to be a chronological
   * relationship necessarily. */
  flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER;

  /* We should be able to switch to a different tree even if the current origin
   * is unconfigured */
  flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED;

  upgrader = rpmostree_sysroot_upgrader_new (sysroot, self->osname, flags,
                                             cancellable, error);
  if (upgrader == NULL)
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  if (!change_upgrader_refspec (sysroot, upgrader,
                                self->refspec, cancellable,
                                &old_refspec, &new_refspec, error))
    goto out;

  progress = ostree_async_progress_new ();
  rpmostreed_transaction_connect_download_progress (transaction, progress);
  rpmostreed_transaction_connect_signature_progress (transaction, repo);

  if (self->revision)
    {
      if (!apply_revision_override (transaction, repo, progress, upgrader,
                                    self->revision, cancellable, error))
        goto out;
    }

  if (!rpmostree_sysroot_upgrader_pull (upgrader, NULL, 0,
					progress, &changed,
					cancellable, error))
    goto out;

  rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

  if (!rpmostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
    goto out;

  if (!self->skip_purge)
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

  if (self->reboot)
    rpmostreed_reboot (cancellable, error);

  ret = TRUE;
out:
  return ret;
}

static void
rebase_transaction_class_init (RebaseTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = rebase_transaction_finalize;

  class->execute = rebase_transaction_execute;
}

static void
rebase_transaction_init (RebaseTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_rebase (GDBusMethodInvocation *invocation,
                                   OstreeSysroot *sysroot,
                                   const char *osname,
                                   const char *refspec,
                                   const char *revision,
                                   gboolean skip_purge,
                                   gboolean reboot,
                                   GCancellable *cancellable,
                                   GError **error)
{
  RebaseTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);
  g_return_val_if_fail (refspec != NULL, NULL);

  self = g_initable_new (rebase_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->refspec = g_strdup (refspec);
      self->revision = g_strdup (revision);
      self->skip_purge = skip_purge;
      self->reboot = reboot;
    }

  return (RpmostreedTransaction *) self;
}

/* ================================ Upgrade/Deploy ================================ */

typedef struct {
  RpmostreedTransaction parent;
  gboolean allow_downgrade; /* Always TRUE for rebase */
  char *osname;
  char *revision; /* NULL for upgrade */
  gboolean reboot;
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
  g_free (self->revision);

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

  RpmOstreeSysrootUpgraderFlags upgrader_flags = 0;
  gboolean changed = FALSE;
  gboolean ret = FALSE;

  self = (DeployTransaction *) transaction;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);

  if (self->allow_downgrade)
    upgrader_flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER;

  upgrader = rpmostree_sysroot_upgrader_new (sysroot, self->osname,
                                             upgrader_flags,
                                             cancellable, error);
  if (upgrader == NULL)
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  progress = ostree_async_progress_new ();
  rpmostreed_transaction_connect_download_progress (transaction, progress);
  rpmostreed_transaction_connect_signature_progress (transaction, repo);

  if (self->revision)
    {
      if (!apply_revision_override (transaction, repo, progress, upgrader,
                                    self->revision, cancellable, error))
        goto out;
    }

  if (!rpmostree_sysroot_upgrader_pull (upgrader, NULL, 0,
                                        progress, &changed, cancellable, error))
    goto out;

  rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

  if (changed)
    {
      if (!rpmostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
        goto out;

      if (self->reboot)
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

RpmostreedTransaction *
rpmostreed_transaction_new_deploy (GDBusMethodInvocation *invocation,
                                   OstreeSysroot *sysroot,
                                   const char *osname,
                                   gboolean allow_downgrade,
                                   const char *revision,
                                   gboolean reboot,
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
      self->allow_downgrade = allow_downgrade;
      self->revision = g_strdup (revision);
      self->reboot = reboot;
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
  RpmOstreeOrigin *origin = NULL; /* Owned by upgrader */
  g_autoptr(GKeyFile) new_origin = NULL;
  g_auto(GStrv) current_initramfs_args = NULL;
  gboolean current_regenerate;

  self = (InitramfsStateTransaction *) transaction;

  sysroot = rpmostreed_transaction_get_sysroot (transaction);

  upgrader = rpmostree_sysroot_upgrader_new (sysroot, self->osname, 0,
                                             cancellable, error);
  if (upgrader == NULL)
    return FALSE;

  origin = rpmostree_sysroot_upgrader_get_origin (upgrader);
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
  new_origin = rpmostree_origin_dup_keyfile (origin);
  rpmostree_origin_set_regenerate_initramfs (new_origin, self->regenerate, self->args);

  if (!rpmostree_sysroot_upgrader_set_origin (upgrader, new_origin, cancellable, error))
    return FALSE;

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
