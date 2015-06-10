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

typedef struct _OSStubClass OSStubClass;
typedef struct _TaskData TaskData;

struct _OSStub
{
  RPMOSTreeOSSkeleton parent_instance;
  guint signal_id;
};

struct _OSStubClass
{
  RPMOSTreeOSSkeletonClass parent_class;
};

struct _TaskData {
  OstreeSysroot *sysroot;
  gboolean sysroot_locked;
  OstreeAsyncProgress *progress;
  RPMOSTreeTransaction *transaction;
  GVariantDict *options;
};

enum {
  PROP_0,
  PROP_SYSROOT
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
    glnx_unref_object GTask *task = G_TASK (res);
    GError *error = NULL;

    GDBusMethodInvocation *invocation = user_data;

    GVariant *result = g_task_propagate_pointer (task, &error);

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
  g_clear_object (&data->progress);
  g_clear_object (&data->transaction);

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
  object_path = g_dbus_interface_skeleton_get_object_path (self);
  if (object_path)
    {
      daemon_unpublish (daemon_get (),
                        object_path,
                        object);
    }

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

static gint
rollback_deployment_index (const gchar *name,
                           OstreeSysroot *ot_sysroot,
                           GError **error)
{
  g_autoptr (GPtrArray) deployments = NULL;
  glnx_unref_object OstreeDeployment *merge_deployment = NULL;

  gint index_to_prepend = -1;
  gint merge_index = -1;
  gint previous_index = -1;
  guint i;

  merge_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
  if (merge_deployment == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No deployments found for os %s", name);
      goto out;
    }

  deployments = ostree_sysroot_get_deployments (ot_sysroot);
  if (deployments->len < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Found %u deployments, at least 2 required for rollback",
                   deployments->len);
      goto out;
    }

  g_assert (merge_deployment != NULL);
  for (i = 0; i < deployments->len; i++)
    {
      if (deployments->pdata[i] == merge_deployment)
        merge_index = i;

      if (g_strcmp0 (ostree_deployment_get_osname (deployments->pdata[i]), name) == 0 &&
              deployments->pdata[i] != merge_deployment &&
              previous_index < 0)
        {
            previous_index = i;
        }
    }

  g_assert (merge_index < deployments->len);
  g_assert (deployments->pdata[merge_index] == merge_deployment);

  /* If merge deployment is not booted assume we are using it. */
  if (merge_index == 0 && previous_index > 0)
      index_to_prepend = previous_index;
  else
      index_to_prepend = merge_index;

out:
  return index_to_prepend;
}


/**
 * refspec_parse_partial:
 * @new_provided_refspec: The provided refspec
 * @base_refspec: The refspec string to base on.
 * @out_refspec: Pointer to the new refspec
 * @error: Pointer to an error pointer.
 *
 * Takes a refspec string and adds any missing bits based on the
 * base_refspec argument. Errors if a full valid refspec can't
 * be derived.
 *
 * Returns: True on sucess.
 */
static gboolean
refspec_parse_partial (const gchar *new_provided_refspec,
                       gchar *base_refspec,
                       gchar **out_refspec,
                       GError **error)
{

  g_autofree gchar *ref = NULL;
  g_autofree gchar *remote = NULL;
  g_autofree gchar *origin_ref = NULL;
  g_autofree gchar *origin_remote = NULL;
  GError *parse_error = NULL;

  gboolean ret = FALSE;

  /* Allow just switching remotes */
  if (g_str_has_suffix (new_provided_refspec, ":"))
    {
      remote = g_strdup (new_provided_refspec);
      remote[strlen (remote) - 1] = '\0';
    }
  else
    {
      if (!ostree_parse_refspec (new_provided_refspec, &remote,
                                 &ref, &parse_error))
        {
          g_set_error_literal (error, RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_INVALID_REFSPEC,
                       parse_error->message);
          g_clear_error (&parse_error);
          goto out;
        }
    }

  if (base_refspec != NULL)
    {
      if (!ostree_parse_refspec (base_refspec, &origin_remote,
                               &origin_ref, &parse_error))
        goto out;
    }

  if (ref == NULL)
    {
      if (origin_ref)
        {
          ref = g_strdup (origin_ref);
        }

      else
        {
          g_set_error (error, RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_INVALID_REFSPEC,
                      "Could not determine default ref to pull.");
          goto out;
        }

    }
  else if (remote == NULL)
    {
      if (origin_remote)
        {
          remote = g_strdup (origin_remote);
        }
      else
        {
          g_set_error (error, RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_INVALID_REFSPEC,
                       "Could not determine default remote to pull.");
          goto out;
        }
    }

  if (g_strcmp0 (origin_remote, remote) == 0 &&
      g_strcmp0 (origin_ref, ref) == 0)
    {
      g_set_error (error, RPM_OSTREED_ERROR,
                   RPM_OSTREED_ERROR_INVALID_REFSPEC,
                   "Old and new refs are equal: %s:%s",
                   remote, ref);
      goto out;
    }

  *out_refspec = g_strconcat (remote, ":", ref, NULL);
  ret = TRUE;

out:
  return ret;
}


static OstreeDeployment *
get_deployment_for_id (OstreeSysroot *sysroot,
                       const gchar *deploy_id)
{
  g_autoptr (GPtrArray) deployments = NULL;
  guint i;

  OstreeDeployment *deployment = NULL;

  deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployments == NULL)
    goto out;

  for (i=0; i<deployments->len; i++)
    {
      g_autofree gchar *id = deployment_generate_id (deployments->pdata[i]);
      if (g_strcmp0 (deploy_id, id) == 0) {
        deployment = g_object_ref (deployments->pdata[i]);
      }
    }

out:
  return deployment;
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

  GVariant *value = NULL; // freed when invoked
  GError *error = NULL; // freed when invoked
  gchar *refspec = data_ptr; // freed by task

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

  g_autofree gchar *comp_ref = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  glnx_unref_object OstreeRepo *ot_repo = NULL;
  glnx_unref_object OstreeDeployment *base_deployment = NULL;

  GVariant *value = NULL; // freed when invoked
  GError *error = NULL; // freed when invoked

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

  GVariant *value = NULL; // freed when invoked
  GError *error = NULL; // freed when invoked
  GPtrArray *compare_refs = data_ptr; // freed by task

  g_return_if_fail (compare_refs->len == 2);

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path ( sysroot_get ()),
                                    cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    &error))
    goto out;

  deployment0 = get_deployment_for_id (ot_sysroot, compare_refs->pdata[0]);
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

  deployment1 = get_deployment_for_id (ot_sysroot, compare_refs->pdata[1]);
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
osstub_upgrade_thread (GTask *task,
                       gpointer source_object,
                       gpointer task_data,
                       GCancellable *cancellable)
{
  RPMOSTreeOS *interface = source_object;
  TaskData *data = task_data;
  gboolean opt_allow_downgrade = FALSE;
  glnx_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autofree char *origin_description = NULL;
  const char *name;
  OstreeSysrootUpgraderPullFlags upgrader_pull_flags = 0;
  gboolean changed = FALSE;
  GError *local_error = NULL;

  /* XXX Fail if option type is wrong? */
  g_variant_dict_lookup (data->options,
                         "allow-downgrade", "b",
                         &opt_allow_downgrade);

  name = rpmostree_os_get_name (interface);

  upgrader = ostree_sysroot_upgrader_new_for_os (data->sysroot, name,
                                                 cancellable, &local_error);
  if (upgrader == NULL)
    goto out;

  origin_description = ostree_sysroot_upgrader_get_origin_description (upgrader);
  /* FIXME Emit origin description in transaction message. */

  if (!ostree_sysroot_get_repo (data->sysroot, &repo,
                                cancellable, &local_error))
    goto out;

  if (opt_allow_downgrade)
    upgrader_pull_flags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

  if (!ostree_sysroot_upgrader_pull (upgrader, 0, upgrader_pull_flags,
                                     data->progress, &changed,
                                     cancellable, &local_error))
    goto out;

  if (changed)
    {
      if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, &local_error))
        goto out;
    }
  else
    {
      /* FIXME Emit "No upgrade available." in transaction message. */
    }

out:
  if (local_error != NULL)
    g_task_return_error (task, local_error);
  else
    g_task_return_boolean (task, TRUE);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
osstub_transaction_complete (GObject *source_object,
                             GAsyncResult *result,
                             gpointer user_data)
{
  TaskData *data;
  gboolean success;
  const char *message;
  GError *local_error = NULL;

  data = g_task_get_task_data (G_TASK (result));

  success = g_task_propagate_boolean (G_TASK (result), &local_error);
  message = (local_error != NULL) ? local_error->message : "";

  rpmostree_transaction_emit_completed (data->transaction, success, message);

  g_clear_error (&local_error);
}


static gboolean
handle_get_deployments_rpm_diff (RPMOSTreeOS *interface,
                                 GDBusMethodInvocation *invocation,
                                 const char *arg_deployid0,
                                 const char *arg_deployid1)
{
  OSStub *self = OSSTUB (interface);
  GPtrArray *compare_refs = NULL; // freed by task
  GTask *task = NULL;

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
                                          GDBusMethodInvocation *invocation)
{
  OSStub *self = OSSTUB (interface);
  GTask *task = NULL;

  glnx_unref_object GCancellable *cancellable = NULL;

  task = g_task_new (self, cancellable,
                     task_result_invoke,
                     invocation);
  g_task_run_in_thread (task, get_upgrade_diff_variant_in_thread);

  return TRUE;
}

static gboolean
osstub_handle_download_update_rpm_diff (RPMOSTreeOS *interface,
                                        GDBusMethodInvocation *invocation,
                                        const char *arg_deployid)
{
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  /* FIXME Do locking here, make sure we have exclusive access. */

  transaction = new_transaction (invocation, cancellable, NULL, &local_error);

  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_download_update_rpm_diff (interface,
                                                      invocation,
                                                      object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
osstub_handle_upgrade (RPMOSTreeOS *interface,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  glnx_unref_object GCancellable *cancellable = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  g_autoptr (GTask) task = NULL;
  TaskData *data;
  gboolean lock_acquired = FALSE;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path ( sysroot_get ()),
                                  cancellable,
                                  &sysroot,
                                  NULL,
                                  &local_error))
    goto out;

  g_return_val_if_fail (sysroot != NULL, FALSE);

  if (!ostree_sysroot_try_lock (sysroot, &lock_acquired, &local_error))
    goto out;

  if (!lock_acquired)
    {
      local_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BUSY,
                                         "System transaction in progress");
      goto out;
    }

  transaction = new_transaction (invocation, cancellable,
                                 &progress, &local_error);

  data = g_slice_new0 (TaskData);
  data->sysroot = g_object_ref (sysroot);
  data->sysroot_locked = TRUE;
  data->progress = g_object_ref (progress);
  data->transaction = g_object_ref (transaction);
  data->options = g_variant_dict_new (arg_options);

  task = g_task_new (interface, cancellable, osstub_transaction_complete, NULL);
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_source_tag (task, osstub_handle_upgrade);
  g_task_set_task_data (task, data, (GDestroyNotify) task_data_free);
  g_task_run_in_thread (task, osstub_upgrade_thread);

out:
  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_upgrade (interface,
                                     invocation,
                                     object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
osstub_handle_rollback (RPMOSTreeOS *interface,
                        GDBusMethodInvocation *invocation)
{
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  /* FIXME Do locking here, make sure we have exclusive access. */

  transaction = new_transaction (invocation, cancellable, NULL, &local_error);

  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_rollback (interface,
                                      invocation,
                                      object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
osstub_handle_clear_rollback_target (RPMOSTreeOS *interface,
                                     GDBusMethodInvocation *invocation)
{
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  /* FIXME Do locking here, make sure we have exclusive access. */

  transaction = new_transaction (invocation, cancellable, NULL, &local_error);

  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_clear_rollback_target (interface,
                                                   invocation,
                                                   object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
osstub_handle_rebase (RPMOSTreeOS *interface,
                      GDBusMethodInvocation *invocation,
                      const char *arg_refspec,
                      const char * const *arg_packages)
{
  /* FIXME */

  return TRUE;
}

static gboolean
osstub_handle_get_cached_rebase_rpm_diff (RPMOSTreeOS *interface,
                                          GDBusMethodInvocation *invocation,
                                          const char *arg_refspec,
                                          const char * const *arg_packages)
{
  OSStub *self = OSSTUB (interface);
  GTask *task = NULL;
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
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  /* FIXME Do locking here, make sure we have exclusive access. */

  transaction = new_transaction (invocation, cancellable, NULL, &local_error);

  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_download_rebase_rpm_diff (interface,
                                                      invocation,
                                                      object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static void
osstub_load_internals (OSStub *self,
                       OstreeSysroot *ot_sysroot)
{
  const gchar *name;

  OstreeDeployment *booted = NULL; // owned by sysroot
  glnx_unref_object  OstreeDeployment *merge_deployment = NULL; // transfered

  glnx_unref_object OstreeRepo *ot_repo = NULL;
  g_autoptr (GPtrArray) deployments = NULL;

  GError *error = NULL;
  GVariant *booted_variant = NULL;
  GVariant *default_variant = NULL;
  GVariant *rollback_variant = NULL;

  gboolean has_cached_updates = FALSE;
  gint rollback_index;
  guint i;

  name = rpmostree_os_get_name (RPMOSTREE_OS (self));
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
      g_autofree gchar *origin_refspec = NULL;

      origin_refspec = deployment_get_refspec (merge_deployment);
      if (!origin_refspec)
        goto out;

      if (!ostree_repo_resolve_rev (ot_repo, origin_refspec,
                                   FALSE, &head, NULL))
        goto out;

      has_cached_updates = g_strcmp0 (ostree_deployment_get_csum(merge_deployment),
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

  rpmostree_os_set_has_cached_update_rpm_diff (RPMOSTREE_OS (self),
                                               has_cached_updates);
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
            const char *name)
{
  const gchar *path;
  OSStub *obj = NULL;

  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  path = utils_generate_object_path (BASE_DBUS_PATH,
                                     name, NULL);

  obj = g_object_new (TYPE_OSSTUB, "name", name, NULL);
  osstub_load_internals (obj, sysroot);
  daemon_publish (daemon_get (), path, FALSE, obj);
  return RPMOSTREE_OS (obj);
}
