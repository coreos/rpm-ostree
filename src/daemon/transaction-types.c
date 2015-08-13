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

#include "transaction-types.h"
#include "transaction.h"
#include "deployment-utils.h"
#include "utils.h"

static gboolean
change_upgrader_refspec (OstreeSysroot *sysroot,
                         OstreeSysrootUpgrader *upgrader,
                         const gchar *refspec,
                         GCancellable *cancellable,
                         gchar **out_old_refspec,
                         gchar **out_new_refspec,
                         GError **error)
{
  gboolean ret = FALSE;

  g_autofree gchar *old_refspec = NULL;
  g_autofree gchar *new_refspec = NULL;
  g_autoptr(GKeyFile) new_origin = NULL;
  GKeyFile *old_origin = NULL; /* owned by deployment */

  old_origin = ostree_sysroot_upgrader_get_origin (upgrader);
  old_refspec = g_key_file_get_string (old_origin, "origin",
                                        "refspec", NULL);

  if (!refspec_parse_partial (refspec,
                              old_refspec,
                              &new_refspec,
                              error))
    goto out;

  if (strcmp (old_refspec, new_refspec) == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Old and new refs are equal: %s", new_refspec);
      goto out;
    }

  new_origin = ostree_sysroot_origin_new_from_refspec (sysroot,
                                                       new_refspec);
  if (!ostree_sysroot_upgrader_set_origin (upgrader, new_origin,
                                           cancellable, error))
    goto out;

  if (out_new_refspec != NULL)
    *out_new_refspec = g_steal_pointer (&new_refspec);

  if (out_old_refspec != NULL)
    *out_old_refspec = g_steal_pointer (&old_refspec);

  ret = TRUE;

out:
  return ret;
}

/* ============================= Package Diff  ============================= */

typedef struct {
  Transaction parent;
  char *osname;
  char *refspec;
} PackageDiffTransaction;

typedef TransactionClass PackageDiffTransactionClass;

GType package_diff_transaction_get_type (void);

G_DEFINE_TYPE (PackageDiffTransaction,
               package_diff_transaction,
               TYPE_TRANSACTION)

static void
package_diff_transaction_finalize (GObject *object)
{
  PackageDiffTransaction *self;

  self = (PackageDiffTransaction *) object;
  g_free (self->osname);
  g_free (self->refspec);

  G_OBJECT_CLASS (package_diff_transaction_parent_class)->finalize (object);
}

static gboolean
package_diff_transaction_execute (Transaction *transaction,
                                  GCancellable *cancellable,
                                  GError **error)
{
  PackageDiffTransaction *self;
  OstreeSysroot *sysroot;

  glnx_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autofree gchar *origin_description = NULL;

  gboolean changed = FALSE;
  gboolean ret = FALSE;

  /* libostree iterates and calls quit on main loop
   * so we need to run in our own context. */
  GMainContext *m_context = g_main_context_new ();
  g_main_context_push_thread_default (m_context);

  self = (PackageDiffTransaction *) transaction;
  sysroot = transaction_get_sysroot (transaction);
  upgrader = ostree_sysroot_upgrader_new_for_os (sysroot, self->osname,
                                                 cancellable, error);
  if (upgrader == NULL)
    goto out;

  if (self->refspec != NULL)
    {
      if (!change_upgrader_refspec (sysroot, upgrader,
                                    self->refspec, cancellable,
                                    NULL, NULL, error))
        goto out;
    }

  origin_description = ostree_sysroot_upgrader_get_origin_description (upgrader);
  if (origin_description != NULL)
    transaction_emit_message_printf (transaction,
                                     "Updating from: %s",
                                     origin_description);

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  progress = ostree_async_progress_new ();
  transaction_connect_download_progress (transaction, progress);
  transaction_connect_signature_progress (transaction, repo);
  if (!ostree_sysroot_upgrader_pull_one_dir (upgrader, "/usr/share/rpm",
                                             0, 0, progress, &changed,
                                             cancellable, error))
    goto out;

  rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

  if (!changed)
    transaction_emit_message_printf (transaction, "No upgrade available.");

  ret = TRUE;

out:
  /* Clean up context */
  g_main_context_pop_thread_default (m_context);
  g_main_context_unref (m_context);

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

Transaction *
transaction_new_package_diff (GDBusMethodInvocation *invocation,
                              OstreeSysroot *sysroot,
                              const char *osname,
                              const char *refspec,
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
                         "sysroot", sysroot,
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->refspec = g_strdup (refspec);
    }

  return (Transaction *) self;
}

/* =============================== Rollback =============================== */

typedef struct {
  Transaction parent;
  char *osname;
} RollbackTransaction;

typedef TransactionClass RollbackTransactionClass;

GType rollback_transaction_get_type (void);

G_DEFINE_TYPE (RollbackTransaction,
               rollback_transaction,
               TYPE_TRANSACTION)

static void
rollback_transaction_finalize (GObject *object)
{
  RollbackTransaction *self;

  self = (RollbackTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (rollback_transaction_parent_class)->finalize (object);
}

static gboolean
rollback_transaction_execute (Transaction *transaction,
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

  sysroot = transaction_get_sysroot (transaction);

  rollback_index = rollback_deployment_index (self->osname, sysroot, error);
  if (rollback_index < 0)
    goto out;

  old_deployments = ostree_sysroot_get_deployments (sysroot);
  new_deployments = g_ptr_array_new_with_free_func (g_object_unref);

  /* build out the reordered array */

  deployment = old_deployments->pdata[rollback_index];
  g_ptr_array_add (new_deployments, g_object_ref (deployment));

  transaction_emit_message_printf (transaction,
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
    ostree_sysroot_write_deployments (sysroot,
                                      new_deployments,
                                      cancellable,
                                      error);

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

Transaction *
transaction_new_rollback (GDBusMethodInvocation *invocation,
                          OstreeSysroot *sysroot,
                          const char *osname,
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
                         "sysroot", sysroot,
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
    }

  return (Transaction *) self;
}

/* ============================ Clear Rollback ============================ */

typedef struct {
  Transaction parent;
  char *osname;
} ClearRollbackTransaction;

typedef TransactionClass ClearRollbackTransactionClass;

GType clear_rollback_transaction_get_type (void);

G_DEFINE_TYPE (ClearRollbackTransaction,
               clear_rollback_transaction,
               TYPE_TRANSACTION)

static void
clear_rollback_transaction_finalize (GObject *object)
{
  ClearRollbackTransaction *self;

  self = (ClearRollbackTransaction *) object;
  g_free (self->osname);

  G_OBJECT_CLASS (clear_rollback_transaction_parent_class)->finalize (object);
}

static gboolean
clear_rollback_transaction_execute (Transaction *transaction,
                                    GCancellable *cancellable,
                                    GError **error)
{
  ClearRollbackTransaction *self;
  OstreeSysroot *sysroot;

  g_autoptr(GPtrArray) deployments = NULL;
  gint rollback_index;
  gboolean ret = FALSE;

  self = (ClearRollbackTransaction *) transaction;

  sysroot = transaction_get_sysroot (transaction);

  rollback_index = rollback_deployment_index (self->osname, sysroot, error);
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

  ostree_sysroot_write_deployments (sysroot,
                                    deployments,
                                    cancellable,
                                    error);

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

Transaction *
transaction_new_clear_rollback (GDBusMethodInvocation *invocation,
                                OstreeSysroot *sysroot,
                                const char *osname,
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
                         "sysroot", sysroot,
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
    }

  return (Transaction *) self;
}

/* ================================ Upgrade ================================ */

typedef struct {
  Transaction parent;
  char *osname;
  char *refspec;
  gboolean allow_downgrade;
  gboolean skip_purge;
} UpgradeTransaction;

typedef TransactionClass UpgradeTransactionClass;

GType upgrade_transaction_get_type (void);

G_DEFINE_TYPE (UpgradeTransaction,
               upgrade_transaction,
               TYPE_TRANSACTION)

static void
upgrade_transaction_finalize (GObject *object)
{
  UpgradeTransaction *self;

  self = (UpgradeTransaction *) object;
  g_free (self->osname);
  g_free (self->refspec);

  G_OBJECT_CLASS (upgrade_transaction_parent_class)->finalize (object);
}

static gboolean
upgrade_transaction_execute (Transaction *transaction,
                             GCancellable *cancellable,
                             GError **error)
{
  UpgradeTransaction *self;
  OstreeSysroot *sysroot;

  glnx_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;

  g_autofree gchar *new_refspec = NULL;
  g_autofree gchar *old_refspec = NULL;
  g_autofree gchar *origin_description = NULL;

  OstreeSysrootUpgraderPullFlags upgrader_pull_flags = 0;

  gboolean changed = FALSE;
  gboolean ret = FALSE;

  /* libostree iterates and calls quit on main loop
   * so we need to run in our own context. */
  GMainContext *m_context = g_main_context_new ();
  g_main_context_push_thread_default (m_context);

  self = (UpgradeTransaction *) transaction;

  sysroot = transaction_get_sysroot (transaction);

  upgrader = ostree_sysroot_upgrader_new_for_os (sysroot, self->osname,
                                                 cancellable, error);
  if (upgrader == NULL)
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  if (self->refspec != NULL)
    {
      if (!change_upgrader_refspec (sysroot, upgrader,
                                    self->refspec, cancellable,
                                    &old_refspec, &new_refspec, error))
        goto out;
    }

  if (self->allow_downgrade || new_refspec)
    upgrader_pull_flags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

  origin_description = ostree_sysroot_upgrader_get_origin_description (upgrader);
  if (origin_description != NULL)
    transaction_emit_message_printf (transaction,
                                     "Updating from: %s",
                                     origin_description);

  progress = ostree_async_progress_new ();
  transaction_connect_download_progress (transaction, progress);
  transaction_connect_signature_progress (transaction, repo);

  if (!ostree_sysroot_upgrader_pull (upgrader, 0, upgrader_pull_flags,
                                     progress, &changed,
                                     cancellable, error))
    goto out;

  rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));

  if (changed)
    {
      if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
        goto out;

      if (!self->skip_purge && old_refspec != NULL)
        {
          g_autofree gchar *remote = NULL;
          g_autofree gchar *ref = NULL;

          if (!ostree_parse_refspec (old_refspec, &remote, &ref, error))
            goto out;

          if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
            goto out;

          ostree_repo_transaction_set_ref (repo, remote, ref, NULL);

          transaction_emit_message_printf (transaction,
                                           "Deleting ref '%s'",
                                           old_refspec);

          if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
            goto out;
        }
    }
  else
    {
      transaction_emit_message_printf (transaction, "No upgrade available.");
    }

  ret = TRUE;

out:
  /* Clean up context */
  g_main_context_pop_thread_default (m_context);
  g_main_context_unref (m_context);

  return ret;
}

static void
upgrade_transaction_class_init (UpgradeTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = upgrade_transaction_finalize;

  class->execute = upgrade_transaction_execute;
}

static void
upgrade_transaction_init (UpgradeTransaction *self)
{
}

Transaction *
transaction_new_upgrade (GDBusMethodInvocation *invocation,
                         OstreeSysroot *sysroot,
                         const char *osname,
                         const char *refspec,
                         gboolean allow_downgrade,
                         gboolean skip_purge,
                         GCancellable *cancellable,
                         GError **error)
{
  UpgradeTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);

  self = g_initable_new (upgrade_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot", sysroot,
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->refspec = g_strdup (refspec);
      self->allow_downgrade = allow_downgrade;
      self->skip_purge = skip_purge;
    }

  return (Transaction *) self;
}

