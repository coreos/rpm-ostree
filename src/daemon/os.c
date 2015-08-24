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
#include "os.h"
#include "utils.h"
#include "transaction.h"
#include "transaction-monitor.h"
#include "transaction-types.h"

typedef struct _OSStubClass OSStubClass;

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

  /* TODO Integrate with PolicyKit via the "g-authorize-method" signal. */

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

/* ---------------------------------------------------------------------------------------------------- */

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
  OSStub *self = OSSTUB (interface);
  glnx_unref_object Transaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  const char *client_address;
  const char *osname;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path (sysroot_get ()),
                                    cancellable, &sysroot, NULL, &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = transaction_new_package_diff (invocation,
                                              sysroot,
                                              osname,
                                              NULL,  /* refspec */
                                              cancellable,
                                              &local_error);

  if (transaction == NULL)
    goto out;

  transaction_monitor_add (self->transaction_monitor, transaction);

  client_address = transaction_get_client_address (transaction);
  rpmostree_os_complete_download_update_rpm_diff (interface, invocation, client_address);

out:
  if (local_error != NULL)
    g_dbus_method_invocation_take_error (invocation, local_error);

  return TRUE;
}

static gboolean
osstub_handle_upgrade (RPMOSTreeOS *interface,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  OSStub *self = OSSTUB (interface);
  glnx_unref_object Transaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GVariantDict options_dict;
  gboolean opt_allow_downgrade = FALSE;
  const char *client_address;
  const char *osname;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path (sysroot_get ()),
                                    cancellable, &sysroot, NULL, &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  /* XXX Fail if option type is wrong? */

  g_variant_dict_init (&options_dict, arg_options);

  g_variant_dict_lookup (&options_dict,
                         "allow-downgrade", "b",
                         &opt_allow_downgrade);

  g_variant_dict_clear (&options_dict);

  transaction = transaction_new_upgrade (invocation,
                                         sysroot,
                                         osname,
                                         NULL,   /* refspec */
                                         opt_allow_downgrade,
                                         FALSE,  /* skip-purge */
                                         cancellable,
                                         &local_error);

  if (transaction == NULL)
    goto out;

  transaction_monitor_add (self->transaction_monitor, transaction);

  client_address = transaction_get_client_address (transaction);
  rpmostree_os_complete_upgrade (interface, invocation, client_address);

out:
  if (local_error != NULL)
    g_dbus_method_invocation_take_error (invocation, local_error);

  return TRUE;
}

static gboolean
osstub_handle_rollback (RPMOSTreeOS *interface,
                        GDBusMethodInvocation *invocation)
{
  OSStub *self = OSSTUB (interface);
  glnx_unref_object Transaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  const char *client_address;
  const char *osname;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path (sysroot_get ()),
                                    cancellable, &sysroot, NULL, &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = transaction_new_rollback (invocation,
                                          sysroot,
                                          osname,
                                          cancellable,
                                          &local_error);

  if (transaction == NULL)
    goto out;

  transaction_monitor_add (self->transaction_monitor, transaction);

  client_address = transaction_get_client_address (transaction);
  rpmostree_os_complete_rollback (interface, invocation, client_address);

out:
  if (local_error != NULL)
    g_dbus_method_invocation_take_error (invocation, local_error);

  return TRUE;
}

static gboolean
osstub_handle_clear_rollback_target (RPMOSTreeOS *interface,
                                     GDBusMethodInvocation *invocation)
{
  OSStub *self = OSSTUB (interface);
  glnx_unref_object Transaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  const char *client_address;
  const char *osname;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path (sysroot_get ()),
                                    cancellable, &sysroot, NULL, &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = transaction_new_clear_rollback (invocation,
                                                sysroot,
                                                osname,
                                                cancellable,
                                                &local_error);

  if (transaction == NULL)
    goto out;

  transaction_monitor_add (self->transaction_monitor, transaction);

  client_address = transaction_get_client_address (transaction);
  rpmostree_os_complete_clear_rollback_target (interface, invocation, client_address);

out:
  if (local_error != NULL)
    g_dbus_method_invocation_take_error (invocation, local_error);

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
  OSStub *self = OSSTUB (interface);
  glnx_unref_object Transaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GVariantDict options_dict;
  gboolean opt_skip_purge = FALSE;
  const char *client_address;
  const char *osname;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path (sysroot_get ()),
                                    cancellable, &sysroot, NULL, &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  /* XXX Fail if option type is wrong? */

  g_variant_dict_init (&options_dict, arg_options);

  g_variant_dict_lookup (&options_dict,
                         "skip-purge", "b",
                         &opt_skip_purge);

  g_variant_dict_clear (&options_dict);

  transaction = transaction_new_upgrade (invocation,
                                         sysroot,
                                         osname,
                                         arg_refspec,
                                         FALSE,  /* allow-downgrade */
                                         opt_skip_purge,
                                         cancellable,
                                         &local_error);

  if (transaction == NULL)
    goto out;

  transaction_monitor_add (self->transaction_monitor, transaction);

  client_address = transaction_get_client_address (transaction);
  rpmostree_os_complete_rebase (interface, invocation, client_address);

out:
  if (local_error != NULL)
    g_dbus_method_invocation_take_error (invocation, local_error);

  return TRUE;
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
  OSStub *self = OSSTUB (interface);
  glnx_unref_object Transaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  const char *client_address;
  const char *osname;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  if (!utils_load_sysroot_and_repo (sysroot_get_sysroot_path (sysroot_get ()),
                                    cancellable, &sysroot, NULL, &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = transaction_new_package_diff (invocation,
                                              sysroot,
                                              osname,
                                              arg_refspec,
                                              cancellable,
                                              &local_error);

  if (transaction == NULL)
    goto out;

  transaction_monitor_add (self->transaction_monitor, transaction);

  client_address = transaction_get_client_address (transaction);
  rpmostree_os_complete_download_rebase_rpm_diff (interface, invocation, client_address);

out:
  if (local_error != NULL)
    g_dbus_method_invocation_take_error (invocation, local_error);

  return TRUE;
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
