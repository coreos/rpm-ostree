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
  g_return_val_if_fail (OSTREE_IS_SYSROOT (ot_sysroot), NULL);

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
osstub_handle_get_cached_update_rpm_diff (RPMOSTreeOS *interface,
                                          GDBusMethodInvocation *invocation,
                                          const char *arg_deployid)
{
  /* FIXME */

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
  /* FIXME */

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
