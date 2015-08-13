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

#include "sysroot.h"
#include "daemon.h"
#include "deployment-utils.h"
#include "rpmostree-package-variants.h"
#include "types.h"
#include "errors.h"
#include "auth.h"
#include "os.h"
#include "utils.h"
#include "transaction.h"
#include "transaction-monitor.h"

typedef struct _OSStubClass OSStubClass;
typedef struct _TaskData TaskData;

struct _OSStub
{
  RPMOSTreeOSSkeleton parent_instance;
  TransactionMonitor *transaction_monitor;
  guint signal_id;
};

struct _OSStubClass
{
  RPMOSTreeOSSkeletonClass parent_class;
};

struct _TaskData {
  OstreeSysroot *sysroot;
  gboolean sysroot_locked;
  gchar *refspec;
  RPMOSTreeTransaction *transaction;
  GVariantDict *options;
  gchar *success_message;
};

static void osstub_iface_init (RPMOSTreeOSIface *iface);

static void osstub_load_internals (OSStub *self,
                                   OstreeSysroot *ot_sysroot);

G_DEFINE_TYPE_WITH_CODE (OSStub, osstub, RPMOSTREE_TYPE_OS_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_OS,
                                                osstub_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

/**
  * task_result_invoke:
  *
  * Completes a GTask where the user_data is
  * an invocation and the task data or error is
  * passed to the invocation when called back.
  */
static void
task_result_invoke (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
    GError *error = NULL;

    GDBusMethodInvocation *invocation = user_data;

    GVariant *result = g_task_propagate_pointer (G_TASK (res), &error);

    if (error)
      g_dbus_method_invocation_take_error (invocation, error);
    else
      g_dbus_method_invocation_return_value (invocation, result);
}


static void
task_data_free (TaskData *data)
{
  if (data->sysroot_locked)
    ostree_sysroot_unlock (data->sysroot);

  g_clear_object (&data->sysroot);
  g_clear_object (&data->transaction);

  g_free (data->refspec);
  g_free (data->success_message);

  g_clear_pointer (&data->options, (GDestroyNotify) g_variant_dict_unref);
}


static void
sysroot_changed (Sysroot *sysroot,
                 OstreeSysroot *ot_sysroot,
                 gpointer user_data)
{
  OSStub *self = OSSTUB (user_data);
  g_return_if_fail (OSTREE_IS_SYSROOT (ot_sysroot));

  osstub_load_internals (self, ot_sysroot);
}

static void
osstub_dispose (GObject *object)
{
  OSStub *self = OSSTUB (object);
  const gchar *object_path;
  object_path = g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON(self));
  if (object_path)
    {
      daemon_unpublish (daemon_get (),
                        object_path,
                        object);
    }

  g_clear_object (&self->transaction_monitor);

  if (self->signal_id > 0)
      g_signal_handler_disconnect (sysroot_get (), self->signal_id);

  self->signal_id = 0;

  G_OBJECT_CLASS (osstub_parent_class)->dispose (object);
}

static void
osstub_init (OSStub *self)
{
}

static void
osstub_constructed (GObject *object)
{
  OSStub *self = OSSTUB (object);
  g_signal_connect (RPMOSTREE_OS(self), "g-authorize-method",
    G_CALLBACK (auth_check_root_or_access_denied), NULL);

  self->signal_id = g_signal_connect (sysroot_get (), "sysroot-updated",
                                      G_CALLBACK (sysroot_changed), self);
  G_OBJECT_CLASS (osstub_parent_class)->constructed (object);
}

static void
osstub_class_init (OSStubClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = osstub_dispose;
  gobject_class->constructed  = osstub_constructed;

}

/* ---------------------------------------------------------------------------------------------------- */

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

static void
set_diff_task_result (GTask *task,
                      GVariant *value,
                      GError *error)
{
  if (error == NULL)
    {
      g_task_return_pointer (task,
                             g_variant_new ("(@a(sua{sv}))", value),
                             NULL);
    }
  else
    {
      g_task_return_error (task, error);
    }
}

static void
get_rebase_diff_variant_in_thread (GTask *task,
                                   gpointer object,
                                   gpointer data_ptr,
                                   GCancellable *cancellable)
{
  RPMOSTreeOS *self = RPMOSTREE_OS (object);
  const gchar *name;

  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  glnx_unref_object OstreeRepo *ot_repo = NULL;
  glnx_unref_object OstreeDeployment *base_deployment = NULL;
  g_autofree gchar *comp_ref = NULL;
  g_autofree gchar *base_refspec = NULL;

  GVariant *value = NULL; /* freed when invoked */
  GError *error = NULL; /* freed when invoked */
  gchar *refspec = data_ptr; /* freed by task */

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path ( sysroot_get ()),
                                    cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    &error))
    goto out;

  name = rpmostree_os_get_name (self);
  base_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
  if (base_deployment == NULL)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No deployments found for os %s", name);
      goto out;
    }

  base_refspec = deployment_get_refspec (base_deployment);
  if (!refspec_parse_partial (refspec,
                              base_refspec,
                              &comp_ref,
                              &error))
    goto out;

  value = rpm_ostree_db_diff_variant (ot_repo,
                                      ostree_deployment_get_csum (base_deployment),
                                      comp_ref,
                                      cancellable,
                                      &error);

out:
  set_diff_task_result (task, value, error);
}

static void
get_upgrade_diff_variant_in_thread (GTask *task,
                                    gpointer object,
                                    gpointer data_ptr,
                                    GCancellable *cancellable)
{
  RPMOSTreeOS *self = RPMOSTREE_OS (object);
  const gchar *name;
  gchar *compare_deployment = data_ptr; /* freed by task */

  g_autofree gchar *comp_ref = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  glnx_unref_object OstreeRepo *ot_repo = NULL;
  glnx_unref_object OstreeDeployment *base_deployment = NULL;

  GVariant *value = NULL; /* freed when invoked */
  GError *error = NULL; /* freed when invoked */

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path ( sysroot_get ()),
                                    cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    &error))
    goto out;

  name = rpmostree_os_get_name (self);
  if (compare_deployment == NULL || compare_deployment[0] == '\0')
    {
      base_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
      if (base_deployment == NULL)
        {
          g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No deployments found for os %s", name);
          goto out;
        }
    }
  else
    {
      base_deployment = deployment_get_for_id (ot_sysroot, compare_deployment);
      if (!base_deployment)
        {
          g_set_error (&error,
                       RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_FAILED,
                       "Invalid deployment id %s",
                       compare_deployment);
          goto out;
        }
    }

  comp_ref = deployment_get_refspec (base_deployment);
  if (!comp_ref)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No upgrade remote found for os %s", name);
      goto out;
    }

  value = rpm_ostree_db_diff_variant (ot_repo,
                                      ostree_deployment_get_csum (base_deployment),
                                      comp_ref,
                                      cancellable,
                                      &error);

out:
  set_diff_task_result (task, value, error);
}

static void
get_deployments_diff_variant_in_thread (GTask *task,
                                        gpointer object,
                                        gpointer data_ptr,
                                        GCancellable *cancellable)
{
  const gchar *ref0;
  const gchar *ref1;

  glnx_unref_object OstreeDeployment *deployment0 = NULL;
  glnx_unref_object OstreeDeployment *deployment1 = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  glnx_unref_object OstreeRepo *ot_repo = NULL;

  GVariant *value = NULL; /* freed when invoked */
  GError *error = NULL; /* freed when invoked */
  GPtrArray *compare_refs = data_ptr; /* freed by task */

  g_return_if_fail (compare_refs->len == 2);

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path ( sysroot_get ()),
                                    cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    &error))
    goto out;

  deployment0 = deployment_get_for_id (ot_sysroot, compare_refs->pdata[0]);
  if (!deployment0)
    {
      gchar *id = compare_refs->pdata[0];
      g_set_error (&error,
                   RPM_OSTREED_ERROR,
                   RPM_OSTREED_ERROR_FAILED,
                   "Invalid deployment id %s",
                   id);
      goto out;
    }
  ref0 = ostree_deployment_get_csum (deployment0);

  deployment1 = deployment_get_for_id (ot_sysroot, compare_refs->pdata[1]);
  if (!deployment1)
    {
      gchar *id = compare_refs->pdata[1];
      g_set_error (&error,
                   RPM_OSTREED_ERROR,
                   RPM_OSTREED_ERROR_FAILED,
                   "Invalid deployment id %s",
                   id);
      goto out;
    }
  ref1 = ostree_deployment_get_csum (deployment1);

  value = rpm_ostree_db_diff_variant (ot_repo,
                                      ref0,
                                      ref1,
                                      cancellable,
                                      &error);

out:
  set_diff_task_result (task, value, error);
}

static void
osstub_pull_dir_thread (GTask *task,
                       gpointer source_object,
                       gpointer task_data,
                       GCancellable *cancellable)
{
  RPMOSTreeOS *interface = source_object;
  TaskData *data = task_data;

  glnx_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autofree gchar *origin_description = NULL;

  const char *name;

  gboolean changed = FALSE;
  GError *local_error = NULL;

  /* libostree iterates and calls quit on main loop
   * so we need to run in our own context. */
  GMainContext *m_context = g_main_context_new ();
  g_main_context_push_thread_default (m_context);

  name = rpmostree_os_get_name (interface);
  upgrader = ostree_sysroot_upgrader_new_for_os (data->sysroot, name,
                                                 cancellable, &local_error);
  if (upgrader == NULL)
    goto out;

  if (data->refspec != NULL)
    {
      if (!change_upgrader_refspec (data->sysroot, upgrader, data->refspec, cancellable,
                                    NULL, NULL, &local_error))
        goto out;
    }

  origin_description = ostree_sysroot_upgrader_get_origin_description (upgrader);
  if (origin_description != NULL)
    transaction_emit_message_printf (data->transaction,
                                     "Updating from: %s",
                                     origin_description);

  if (!ostree_sysroot_get_repo (data->sysroot, &repo,
                                cancellable, &local_error))
    goto out;

  progress = ostree_async_progress_new ();
  transaction_connect_download_progress (data->transaction, progress);
  transaction_connect_signature_progress (data->transaction, repo);
  if (!ostree_sysroot_upgrader_pull_one_dir (upgrader, "/usr/share/rpm",
                                             0, 0, progress, &changed,
                                             cancellable, &local_error))
    goto out;

  rpmostree_transaction_emit_progress_end (data->transaction);

  if (!changed)
    data->success_message = g_strdup ("No upgrade available.");

out:
  /* Clean up context */
  g_main_context_pop_thread_default (m_context);
  g_main_context_unref (m_context);

  if (local_error != NULL)
    g_task_return_error (task, local_error);
  else
    g_task_return_boolean (task, TRUE);
}

static void
osstub_upgrade_thread (GTask *task,
                       gpointer source_object,
                       gpointer task_data,
                       GCancellable *cancellable)
{
  RPMOSTreeOS *interface = source_object;
  TaskData *data = task_data;

  glnx_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;

  g_autofree gchar *new_refspec = NULL;
  g_autofree gchar *old_refspec = NULL;
  g_autofree gchar *origin_description = NULL;

  const char *name;
  GError *local_error = NULL;

  OstreeSysrootUpgraderPullFlags upgrader_pull_flags = 0;

  gboolean opt_allow_downgrade = FALSE;
  gboolean skip_purge = FALSE;
  gboolean changed = FALSE;

  /* libostree iterates and calls quit on main loop
   * so we need to run in our own context. */
  GMainContext *m_context = g_main_context_new ();
  g_main_context_push_thread_default (m_context);

  /* XXX Fail if option type is wrong? */
  g_variant_dict_lookup (data->options,
                         "allow-downgrade", "b",
                         &opt_allow_downgrade);

  name = rpmostree_os_get_name (interface);

	upgrader = ostree_sysroot_upgrader_new_for_os (data->sysroot, name,
                                                 cancellable, &local_error);
  if (upgrader == NULL)
    goto out;

  if (!ostree_sysroot_get_repo (data->sysroot, &repo,
                                cancellable, &local_error))
    goto out;

  if (data->refspec != NULL)
    {
      if (!change_upgrader_refspec (data->sysroot, upgrader, data->refspec, cancellable,
                                    &old_refspec, &new_refspec, &local_error))
        goto out;

      g_variant_dict_lookup (data->options,
                             "skip-purge", "b",
                             &skip_purge);
    }

  if (opt_allow_downgrade || new_refspec)
    upgrader_pull_flags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

  origin_description = ostree_sysroot_upgrader_get_origin_description (upgrader);
  if (origin_description != NULL)
    transaction_emit_message_printf (data->transaction,
                                     "Updating from: %s",
                                     origin_description);

  progress = ostree_async_progress_new ();
  transaction_connect_download_progress (data->transaction, progress);
  transaction_connect_signature_progress (data->transaction, repo);

  if (!ostree_sysroot_upgrader_pull (upgrader, 0, upgrader_pull_flags,
                                     progress, &changed,
                                     cancellable, &local_error))
    goto out;

  rpmostree_transaction_emit_progress_end (data->transaction);

  if (changed)
    {
      if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, &local_error))
        goto out;

      if (!skip_purge && old_refspec != NULL)
        {
          g_autofree gchar *remote = NULL;
          g_autofree gchar *ref = NULL;

          if (!ostree_parse_refspec (old_refspec, &remote,
                                     &ref, &local_error))
            goto out;

          if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, &local_error))
            goto out;

          ostree_repo_transaction_set_ref (repo, remote, ref, NULL);

          transaction_emit_message_printf (data->transaction,
                                           "Deleting ref '%s'",
                                           old_refspec);

          if (!ostree_repo_commit_transaction (repo, NULL, cancellable, &local_error))
            goto out;
        }
    }
  else
    {
      data->success_message = g_strdup ("No upgrade available.");
    }

out:
  /* Clean up context */
  g_main_context_pop_thread_default (m_context);
  g_main_context_unref (m_context);

  if (local_error != NULL)
    g_task_return_error (task, local_error);
  else
    g_task_return_boolean (task, TRUE);
}

static void
osstub_rollback_thread (GTask         *task,
                        gpointer       source_object,
                        gpointer       task_data,
                        GCancellable  *cancellable)
{
  RPMOSTreeOS *interface = source_object;
  TaskData *data = task_data;
  const char *name;
  const char *csum;
  g_autoptr(GPtrArray) deployments = NULL;
  g_autoptr(GPtrArray) new_deployments = NULL;
  GError *error = NULL;

  gint rollback_index;
  guint i;
  gint deployserial;

  name = rpmostree_os_get_name (interface);

  rollback_index = rollback_deployment_index (name, data->sysroot, &error);
  if (rollback_index < 0)
    goto out;

  deployments = ostree_sysroot_get_deployments (data->sysroot);
  new_deployments = g_ptr_array_new_with_free_func (g_object_unref);

  /* build out the reordered array */
  g_ptr_array_add (new_deployments, g_object_ref (deployments->pdata[rollback_index]));
  for (i = 0; i < deployments->len; i++)
  {
    if (i == rollback_index)
      continue;

    g_ptr_array_add (new_deployments, g_object_ref (deployments->pdata[i]));
  }

  csum = ostree_deployment_get_csum (deployments->pdata[rollback_index]);
  deployserial = ostree_deployment_get_deployserial (deployments->pdata[rollback_index]);
  transaction_emit_message_printf (data->transaction,
                                   "Moving '%s.%d' to be first deployment",
                                   csum, deployserial);

  /* if default changed write it */
  if (deployments->pdata[0] != new_deployments->pdata[0])
    ostree_sysroot_write_deployments (data->sysroot,
                                      new_deployments,
                                      cancellable,
                                      &error);

out:
  if (error == NULL)
      g_task_return_boolean (task, TRUE);
  else
      g_task_return_error (task, error);
}

static void
osstub_clear_rollback_thread (GTask         *task,
                               gpointer       source_object,
                               gpointer       task_data,
                               GCancellable  *cancellable)
{
  RPMOSTreeOS *interface = source_object;
  TaskData *data = task_data;
  const char *name;

  g_autoptr(GPtrArray) deployments = NULL;
  g_autoptr(GPtrArray) new_deployments = NULL;

  GError *error = NULL;

  gint rollback_index;

  name = rpmostree_os_get_name (interface);

  rollback_index = rollback_deployment_index (name, data->sysroot, &error);
  if (rollback_index < 0)
    goto out;

  deployments = ostree_sysroot_get_deployments (data->sysroot);

  if (deployments->pdata[rollback_index] == ostree_sysroot_get_booted_deployment (data->sysroot))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Cannot undeploy currently booted deployment %i",
                   rollback_index);
      goto out;
    }

  g_ptr_array_remove_index (deployments, rollback_index);

  ostree_sysroot_write_deployments (data->sysroot,
                                    deployments,
                                    cancellable,
                                    &error);

out:
  if (error == NULL)
      g_task_return_boolean (task, TRUE);
  else
      g_task_return_error (task, error);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
acquire_sysroot_lock (GCancellable *cancellable,
                      OstreeSysroot **out_sysroot,
                      GError **error)
{
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  gboolean lock_acquired = FALSE;

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path ( sysroot_get ()),
                                    cancellable,
                                    &sysroot,
                                    NULL,
                                    error))
    goto out;

  g_return_val_if_fail (sysroot != NULL, FALSE);

  if (!ostree_sysroot_try_lock (sysroot, &lock_acquired, error))
    goto out;

  if (!lock_acquired)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "System transaction in progress");
      goto out;
    }

  *out_sysroot = g_steal_pointer (&sysroot);

out:
  return lock_acquired;
}

static void
respond_to_transaction_invocation (GDBusMethodInvocation *invocation,
                                   RPMOSTreeTransaction *transaction,
                                   GError *error)
{
  if (error == NULL)
    {
      const char *object_path;

      g_return_if_fail (transaction != NULL);

      object_path = g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON(transaction));
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(o)", object_path));
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, error);
    }
}

static void
osstub_transaction_done_cb (GObject *source_object,
                            GAsyncResult *result,
                            gpointer user_data)
{
  TaskData *data;
  GError *local_error = NULL;

  data = g_task_get_task_data (G_TASK (result));

  if (g_task_propagate_boolean (G_TASK (result), &local_error))
    {
      sysroot_emit_update (sysroot_get (), data->sysroot);
      transaction_done (data->transaction, TRUE, data->success_message);
    }
  else
    {
      transaction_done (data->transaction, FALSE, local_error->message);
      g_clear_error (&local_error);
    }
}

static gboolean
osstub_handle_pull_dir (RPMOSTreeOS *interface,
                        GDBusMethodInvocation *invocation,
                        gchar *refspec)
{
  OSStub *os = OSSTUB (interface);
  g_autoptr(GTask) task = NULL;
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  TaskData *data;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  if (!acquire_sysroot_lock (cancellable, &sysroot, &local_error))
    goto out;

  transaction = transaction_monitor_new_transaction (os->transaction_monitor,
                                                     invocation, cancellable,
                                                     &local_error);

  if (transaction == NULL)
    goto out;

  data = g_slice_new0 (TaskData);
  data->sysroot = g_object_ref (sysroot);
  data->sysroot_locked = TRUE;
  data->refspec = refspec;
  data->transaction = g_object_ref (transaction);
  data->options = NULL;

  task = g_task_new (interface, cancellable, osstub_transaction_done_cb, NULL);
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_source_tag (task, osstub_handle_pull_dir);
  g_task_set_task_data (task, data, (GDestroyNotify) task_data_free);
  g_task_run_in_thread (task, osstub_pull_dir_thread);

out:
  respond_to_transaction_invocation (invocation, transaction, local_error);
  return TRUE;
}

static gboolean
osstub_handle_deploy (RPMOSTreeOS *interface,
                      GDBusMethodInvocation *invocation,
                      GVariant *arg_options,
                      gchar *refspec)
{
  OSStub *os = OSSTUB (interface);
  glnx_unref_object GCancellable *cancellable = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  g_autoptr(GTask) task = NULL;
  TaskData *data;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  if (!acquire_sysroot_lock (cancellable, &sysroot, &local_error))
    goto out;

  transaction = transaction_monitor_new_transaction (os->transaction_monitor,
                                                     invocation, cancellable,
                                                     &local_error);

  if (transaction == NULL)
    goto out;

  data = g_slice_new0 (TaskData);
  data->sysroot = g_object_ref (sysroot);
  data->sysroot_locked = TRUE;
  data->refspec = refspec;
  data->transaction = g_object_ref (transaction);
  data->options = g_variant_dict_new (arg_options);

  task = g_task_new (interface, cancellable, osstub_transaction_done_cb, NULL);
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_source_tag (task, osstub_handle_deploy);
  g_task_set_task_data (task, data, (GDestroyNotify) task_data_free);
  g_task_run_in_thread (task, osstub_upgrade_thread);

out:
  respond_to_transaction_invocation (invocation, transaction, local_error);
  return TRUE;
}

static gboolean
handle_get_deployments_rpm_diff (RPMOSTreeOS *interface,
                                 GDBusMethodInvocation *invocation,
                                 const char *arg_deployid0,
                                 const char *arg_deployid1)
{
  OSStub *self = OSSTUB (interface);
  GPtrArray *compare_refs = NULL; /* freed by task */
  g_autoptr(GTask) task = NULL;

  glnx_unref_object GCancellable *cancellable = NULL;

  compare_refs = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (compare_refs, g_strdup (arg_deployid0));
  g_ptr_array_add (compare_refs, g_strdup (arg_deployid1));

  task = g_task_new (self, cancellable,
                     task_result_invoke,
                     invocation);
  g_task_set_task_data (task,
                        compare_refs,
                        (GDestroyNotify) g_ptr_array_unref);
  g_task_run_in_thread (task, get_deployments_diff_variant_in_thread);

  return TRUE;
}

static gboolean
osstub_handle_get_cached_update_rpm_diff (RPMOSTreeOS *interface,
                                          GDBusMethodInvocation *invocation,
                                          const char *arg_deployid)
{
  OSStub *self = OSSTUB (interface);
  g_autoptr(GTask) task = NULL;

  glnx_unref_object GCancellable *cancellable = NULL;

  task = g_task_new (self, cancellable,
                     task_result_invoke,
                     invocation);
  g_task_set_task_data (task,
                        g_strdup (arg_deployid),
                        g_free);
  g_task_run_in_thread (task, get_upgrade_diff_variant_in_thread);

  return TRUE;
}

static gboolean
osstub_handle_download_update_rpm_diff (RPMOSTreeOS *interface,
                                        GDBusMethodInvocation *invocation)
{
  return osstub_handle_pull_dir (interface, invocation, NULL);
}

static gboolean
osstub_handle_upgrade (RPMOSTreeOS *interface,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  return osstub_handle_deploy (interface, invocation, arg_options, NULL);
}

static gboolean
osstub_handle_rollback (RPMOSTreeOS *interface,
                        GDBusMethodInvocation *invocation)
{
  OSStub *os = OSSTUB (interface);
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  g_autoptr(GTask) task = NULL;
  TaskData *data;

  cancellable = g_cancellable_new ();

  if (!acquire_sysroot_lock (cancellable, &sysroot, &local_error))
    goto out;

  transaction = transaction_monitor_new_transaction (os->transaction_monitor,
                                                     invocation, cancellable,
                                                     &local_error);

  if (transaction == NULL)
    goto out;

  data = g_slice_new0 (TaskData);
  data->sysroot = g_object_ref (sysroot);
  data->sysroot_locked = TRUE;
  data->refspec = NULL;
  data->transaction = g_object_ref (transaction);
  data->options = NULL;

  task = g_task_new (interface, cancellable, osstub_transaction_done_cb, NULL);
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_source_tag (task, osstub_handle_rollback);
  g_task_set_task_data (task, data, (GDestroyNotify) task_data_free);
  g_task_run_in_thread (task, osstub_rollback_thread);

out:
  respond_to_transaction_invocation (invocation, transaction, local_error);
  return TRUE;
}

static gboolean
osstub_handle_clear_rollback_target (RPMOSTreeOS *interface,
                                     GDBusMethodInvocation *invocation)
{
  OSStub *os = OSSTUB (interface);
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  g_autoptr(GTask) task = NULL;
  TaskData *data;

  cancellable = g_cancellable_new ();

  if (!acquire_sysroot_lock (cancellable, &sysroot, &local_error))
    goto out;

  transaction = transaction_monitor_new_transaction (os->transaction_monitor,
                                                     invocation, cancellable,
                                                     &local_error);

  if (transaction == NULL)
    goto out;

  data = g_slice_new0 (TaskData);
  data->sysroot = g_object_ref (sysroot);
  data->sysroot_locked = TRUE;
  data->refspec = NULL;
  data->transaction = g_object_ref (transaction);
  data->options = NULL;

  task = g_task_new (interface, cancellable, osstub_transaction_done_cb, NULL);
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_source_tag (task, osstub_handle_clear_rollback_target);
  g_task_set_task_data (task, data, (GDestroyNotify) task_data_free);
  g_task_run_in_thread (task, osstub_clear_rollback_thread);

out:
  respond_to_transaction_invocation (invocation, transaction, local_error);
  return TRUE;
}

static gboolean
osstub_handle_rebase (RPMOSTreeOS *interface,
                      GDBusMethodInvocation *invocation,
                      GVariant *arg_options,
                      const char *arg_refspec,
                      const char * const *arg_packages)
{
  /* TODO: Totally ignoring arg_packages for now */
  return osstub_handle_deploy (interface, invocation,
                               arg_options, g_strdup (arg_refspec));
}

static gboolean
osstub_handle_get_cached_rebase_rpm_diff (RPMOSTreeOS *interface,
                                          GDBusMethodInvocation *invocation,
                                          const char *arg_refspec,
                                          const char * const *arg_packages)
{
  OSStub *self = OSSTUB (interface);
  g_autoptr(GTask) task = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;

  /* TODO: Totally ignoring packages for now */
  task = g_task_new (self, cancellable,
                     task_result_invoke,
                     invocation);
  g_task_set_task_data (task,
                        g_strdup (arg_refspec),
                        g_free);
  g_task_run_in_thread (task, get_rebase_diff_variant_in_thread);

  return TRUE;
}

static gboolean
osstub_handle_download_rebase_rpm_diff (RPMOSTreeOS *interface,
                                        GDBusMethodInvocation *invocation,
                                        const char *arg_refspec,
                                        const char * const *arg_packages)
{
  /* TODO: Totally ignoring arg_packages for now */
  return osstub_handle_pull_dir (interface, invocation, g_strdup (arg_refspec));
}

static void
osstub_load_internals (OSStub *self,
                       OstreeSysroot *ot_sysroot)
{
  const gchar *name;

  OstreeDeployment *booted = NULL; /* owned by sysroot */
  glnx_unref_object  OstreeDeployment *merge_deployment = NULL; /* transfered */

  glnx_unref_object OstreeRepo *ot_repo = NULL;
  g_autoptr(GPtrArray) deployments = NULL;
  g_autofree gchar *origin_refspec = NULL;

  GError *error = NULL;
  GVariant *booted_variant = NULL;
  GVariant *default_variant = NULL;
  GVariant *rollback_variant = NULL;

  gboolean has_cached_updates = FALSE;
  gint rollback_index;
  guint i;

  name = rpmostree_os_get_name (RPMOSTREE_OS (self));
  g_debug ("loading %s", name);

  if (!ostree_sysroot_get_repo (ot_sysroot,
                                &ot_repo,
                                NULL,
                                &error))
    goto out;

  deployments = ostree_sysroot_get_deployments (ot_sysroot);
  if (deployments == NULL)
    goto out;

  for (i=0; i<deployments->len; i++)
    {
      if (g_strcmp0 (ostree_deployment_get_osname (deployments->pdata[i]), name) == 0)
        {
          default_variant = deployment_generate_variant (deployments->pdata[i],
                                                         ot_repo);
          break;
        }
    }

  booted = ostree_sysroot_get_booted_deployment (ot_sysroot);
  if (booted && g_strcmp0 (ostree_deployment_get_osname (booted),
                           name) == 0)
    {
      booted_variant = deployment_generate_variant (booted,
                                                    ot_repo);

    }

  rollback_index = rollback_deployment_index (name, ot_sysroot, &error);
  if (error == NULL)
    {
      rollback_variant = deployment_generate_variant (deployments->pdata[rollback_index],
                                                      ot_repo);
    }

  merge_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
  if (merge_deployment)
    {
      g_autofree gchar *head = NULL;

      origin_refspec = deployment_get_refspec (merge_deployment);
      if (!origin_refspec)
        goto out;

      if (!ostree_repo_resolve_rev (ot_repo, origin_refspec,
                                   FALSE, &head, NULL))
        goto out;

      has_cached_updates = g_strcmp0 (ostree_deployment_get_csum (merge_deployment),
                                      head) != 0;
    }

out:
  g_clear_error (&error);

  if (!booted_variant)
    booted_variant = deployment_generate_blank_variant ();
  rpmostree_os_set_booted_deployment (RPMOSTREE_OS (self),
                                      booted_variant);

  if (!default_variant)
    default_variant = deployment_generate_blank_variant ();
  rpmostree_os_set_default_deployment (RPMOSTREE_OS (self),
                                       default_variant);

  if (!rollback_variant)
    rollback_variant = deployment_generate_blank_variant ();
  rpmostree_os_set_rollback_deployment (RPMOSTREE_OS (self),
                                        rollback_variant);

  rpmostree_os_set_has_cached_update_rpm_diff (RPMOSTREE_OS (self),
                                               has_cached_updates);

  rpmostree_os_set_upgrade_origin (RPMOSTREE_OS (self),
                                   origin_refspec ? origin_refspec : "");
  g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON (self));
}

static void
osstub_iface_init (RPMOSTreeOSIface *iface)
{
  iface->handle_get_cached_update_rpm_diff = osstub_handle_get_cached_update_rpm_diff;
  iface->handle_get_deployments_rpm_diff = handle_get_deployments_rpm_diff;
  iface->handle_download_update_rpm_diff   = osstub_handle_download_update_rpm_diff;
  iface->handle_upgrade                    = osstub_handle_upgrade;
  iface->handle_rollback                   = osstub_handle_rollback;
  iface->handle_clear_rollback_target      = osstub_handle_clear_rollback_target;
  iface->handle_rebase                     = osstub_handle_rebase;
  iface->handle_get_cached_rebase_rpm_diff = osstub_handle_get_cached_rebase_rpm_diff;
  iface->handle_download_rebase_rpm_diff   = osstub_handle_download_rebase_rpm_diff;
}

/* ---------------------------------------------------------------------------------------------------- */

RPMOSTreeOS *
osstub_new (OstreeSysroot *sysroot,
            const char *name,
            TransactionMonitor *monitor)
{
  const gchar *path;
  OSStub *obj = NULL;

  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (name != NULL, NULL);
  g_return_val_if_fail (IS_TRANSACTION_MONITOR (monitor), NULL);

  path = utils_generate_object_path (BASE_DBUS_PATH,
                                     name, NULL);

  obj = g_object_new (TYPE_OSSTUB, "name", name, NULL);

  /* FIXME Make this a construct-only property? */
  obj->transaction_monitor = g_object_ref (monitor);

  osstub_load_internals (obj, sysroot);
  daemon_publish (daemon_get (), path, FALSE, obj);
  return RPMOSTREE_OS (obj);
}
