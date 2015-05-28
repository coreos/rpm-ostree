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

#include "libglnx.h"

#include "types.h"
#include "os.h"
#include "utils.h"

typedef struct _OSStubClass OSStubClass;
typedef struct _TaskData TaskData;

struct _OSStub
{
  RPMOSTreeOSSkeleton parent_instance;

  GWeakRef sysroot;
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
osstub_set_sysroot (OSStub *self,
                    OstreeSysroot *sysroot)
{
  g_weak_ref_set (&self->sysroot, sysroot);
}

static void
osstub_set_property (GObject *object,
                     guint property_id,
                     const GValue *value,
                     GParamSpec *pspec)
{
  OSStub *self = OSSTUB (object);

  switch (property_id)
    {
      case PROP_SYSROOT:
        osstub_set_sysroot (self, g_value_get_object (value));
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
osstub_get_property (GObject *object,
                     guint property_id,
                     GValue *value,
                     GParamSpec *pspec)
{
  OSStub *self = OSSTUB (object);

  switch (property_id)
    {
      case PROP_SYSROOT:
        g_value_take_object (value, osstub_ref_sysroot (self));
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
osstub_dispose (GObject *object)
{
  OSStub *self = OSSTUB (object);

  g_weak_ref_clear (&self->sysroot);

  G_OBJECT_CLASS (osstub_parent_class)->dispose (object);
}

static void
osstub_init (OSStub *self)
{
  g_weak_ref_init (&self->sysroot, NULL);
}

static void
osstub_class_init (OSStubClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->set_property = osstub_set_property;
  gobject_class->get_property = osstub_get_property;
  gobject_class->dispose = osstub_dispose;

  g_object_class_install_property (gobject_class,
                                   PROP_SYSROOT,
                                   g_param_spec_object ("sysroot",
                                                        NULL,
                                                        NULL,
                                                        OSTREE_TYPE_SYSROOT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

/* ---------------------------------------------------------------------------------------------------- */

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
  g_autoptr(GTask) task = NULL;
  TaskData *data;
  gboolean lock_acquired = FALSE;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  sysroot = osstub_ref_sysroot (OSSTUB (interface));
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
osstub_iface_init (RPMOSTreeOSIface *iface)
{
  iface->handle_get_cached_update_rpm_diff = osstub_handle_get_cached_update_rpm_diff;
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
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  return g_object_new (TYPE_OSSTUB, "sysroot", sysroot, "name", name, NULL);
}

OstreeSysroot *
osstub_ref_sysroot (OSStub *self)
{
  g_return_val_if_fail (IS_OSSTUB (self), NULL);

  return g_weak_ref_get (&self->sysroot);
}
