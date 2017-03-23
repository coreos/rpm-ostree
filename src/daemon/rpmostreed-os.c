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

#include "rpmostreed-sysroot.h"
#include "rpmostreed-daemon.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostree-package-variants.h"
#include "rpmostreed-errors.h"
#include "rpmostree-origin.h"
#include "rpmostreed-os.h"
#include "rpmostreed-utils.h"
#include "rpmostree-util.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-transaction-monitor.h"
#include "rpmostreed-transaction-types.h"

typedef struct _RpmostreedOSClass RpmostreedOSClass;

struct _RpmostreedOS
{
  RPMOSTreeOSSkeleton parent_instance;
  RpmostreedTransactionMonitor *transaction_monitor;
  guint signal_id;
};

struct _RpmostreedOSClass
{
  RPMOSTreeOSSkeletonClass parent_class;
};

static void rpmostreed_os_iface_init (RPMOSTreeOSIface *iface);

static gboolean rpmostreed_os_load_internals (RpmostreedOS *self, GError **error);

G_DEFINE_TYPE_WITH_CODE (RpmostreedOS,
                         rpmostreed_os,
                         RPMOSTREE_TYPE_OS_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_OS,
                                                rpmostreed_os_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
sysroot_changed (RpmostreedSysroot *sysroot,
                 gpointer user_data)
{
  RpmostreedOS *self = RPMOSTREED_OS (user_data);
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

  if (!rpmostreed_os_load_internals (self, error))
      goto out;
  
 out:
  if (local_error)
    g_warning ("%s", local_error->message);
}

static void
os_dispose (GObject *object)
{
  RpmostreedOS *self = RPMOSTREED_OS (object);
  const gchar *object_path;

  object_path = g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON(self));
  if (object_path != NULL)
    {
      rpmostreed_daemon_unpublish (rpmostreed_daemon_get (),
                                   object_path, object);
    }

  g_clear_object (&self->transaction_monitor);

  if (self->signal_id > 0)
      g_signal_handler_disconnect (rpmostreed_sysroot_get (), self->signal_id);

  self->signal_id = 0;

  G_OBJECT_CLASS (rpmostreed_os_parent_class)->dispose (object);
}

static void
os_constructed (GObject *object)
{
  RpmostreedOS *self = RPMOSTREED_OS (object);

  /* TODO Integrate with PolicyKit via the "g-authorize-method" signal. */

  self->signal_id = g_signal_connect (rpmostreed_sysroot_get (),
                                      "updated",
                                      G_CALLBACK (sysroot_changed), self);
  G_OBJECT_CLASS (rpmostreed_os_parent_class)->constructed (object);
}

static void
rpmostreed_os_class_init (RpmostreedOSClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = os_dispose;
  gobject_class->constructed  = os_constructed;

}

static void
rpmostreed_os_init (RpmostreedOS *self)
{
}

/* ---------------------------------------------------------------------------------------------------- */

static GVariant *
new_variant_diff_result (GVariant *diff,
                         GVariant *details)
{
  return g_variant_new ("(@a(sua{sv})@a{sv})", diff, details);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
os_handle_get_deployments_rpm_diff (RPMOSTreeOS *interface,
                                    GDBusMethodInvocation *invocation,
                                    const char *arg_deployid0,
                                    const char *arg_deployid1)
{
  g_autoptr(GCancellable) cancellable = NULL;
  RpmostreedSysroot *global_sysroot;
  glnx_unref_object OstreeDeployment *deployment0 = NULL;
  glnx_unref_object OstreeDeployment *deployment1 = NULL;
  OstreeSysroot *ot_sysroot = NULL;
  OstreeRepo *ot_repo = NULL;
  GVariant *value = NULL; /* freed when invoked */
  GError *local_error = NULL;
  const gchar *ref0;
  const gchar *ref1;

  global_sysroot = rpmostreed_sysroot_get ();

  ot_sysroot = rpmostreed_sysroot_get_root (global_sysroot);
  ot_repo = rpmostreed_sysroot_get_repo (global_sysroot);

  deployment0 = rpmostreed_deployment_get_for_id (ot_sysroot, arg_deployid0);
  if (!deployment0)
    {
      local_error = g_error_new (RPM_OSTREED_ERROR,
                                 RPM_OSTREED_ERROR_FAILED,
                                 "Invalid deployment id %s",
                                 arg_deployid0);
      goto out;
    }
  ref0 = ostree_deployment_get_csum (deployment0);

  deployment1 = rpmostreed_deployment_get_for_id (ot_sysroot, arg_deployid1);
  if (!deployment1)
    {
      local_error = g_error_new (RPM_OSTREED_ERROR,
                                 RPM_OSTREED_ERROR_FAILED,
                                 "Invalid deployment id %s",
                                 arg_deployid1);
      goto out;
    }
  ref1 = ostree_deployment_get_csum (deployment1);

  value = rpm_ostree_db_diff_variant (ot_repo,
                                      ref0,
                                      ref1,
                                      cancellable,
                                      &local_error);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      g_dbus_method_invocation_return_value (invocation,
					     g_variant_new ("(@a(sua{sv}))", value));
    }

  return TRUE;
}

static gboolean
os_handle_get_cached_update_rpm_diff (RPMOSTreeOS *interface,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_deployid)
{
  RpmostreedSysroot *global_sysroot;
  const gchar *name;
  g_autoptr(RpmOstreeOrigin) origin = NULL;
  OstreeSysroot *ot_sysroot = NULL;
  OstreeRepo *ot_repo = NULL;
  glnx_unref_object OstreeDeployment *base_deployment = NULL;
  GCancellable *cancellable = NULL;
  GVariant *value = NULL; /* freed when invoked */
  GVariant *details = NULL; /* freed when invoked */
  GError *local_error = NULL;

  global_sysroot = rpmostreed_sysroot_get ();

  ot_sysroot = rpmostreed_sysroot_get_root (global_sysroot);
  ot_repo = rpmostreed_sysroot_get_repo (global_sysroot);

  name = rpmostree_os_get_name (interface);
  if (arg_deployid == NULL || arg_deployid[0] == '\0')
    {
      base_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
      if (base_deployment == NULL)
        {
          local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "No deployments found for os %s", name);
          goto out;
        }
    }
  else
    {
      base_deployment = rpmostreed_deployment_get_for_id (ot_sysroot, arg_deployid);
      if (!base_deployment)
        {
          local_error = g_error_new (RPM_OSTREED_ERROR,
                                     RPM_OSTREED_ERROR_FAILED,
                                     "Invalid deployment id %s",
                                     arg_deployid);
          goto out;
        }
    }

  origin = rpmostree_origin_parse_deployment (base_deployment, &local_error);
  if (!origin)
    goto out;

  value = rpm_ostree_db_diff_variant (ot_repo,
                                      ostree_deployment_get_csum (base_deployment),
                                      rpmostree_origin_get_refspec (origin),
                                      cancellable,
                                      &local_error);
  if (value == NULL)
    goto out;

  details = rpmostreed_commit_generate_cached_details_variant (base_deployment,
                                                               ot_repo,
                                                               rpmostree_origin_get_refspec (origin),
							       &local_error);
  if (!details)
    goto out;

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      g_dbus_method_invocation_return_value (invocation,
					     new_variant_diff_result (value, details));
    }

  return TRUE;
}

static RpmostreedTransaction *
merge_compatible_txn (RpmostreedOS *self,
                      GDBusMethodInvocation *invocation)
{
  glnx_unref_object RpmostreedTransaction *transaction = NULL;

  /* If a compatible transaction is in progress, share its bus address. */
  transaction = rpmostreed_transaction_monitor_ref_active_transaction (self->transaction_monitor);
  if (transaction != NULL)
    {
      if (rpmostreed_transaction_is_compatible (transaction, invocation))
        return g_steal_pointer (&transaction);
    }

  return NULL;
}

static gboolean
os_handle_download_update_rpm_diff (RPMOSTreeOS *interface,
                                    GDBusMethodInvocation *invocation)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_package_diff (invocation,
                                                         ot_sysroot,
                                                         osname,
                                                         NULL,  /* refspec */
                                                         NULL,  /* revision */
                                                         cancellable,
                                                         &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_download_update_rpm_diff (interface, invocation, client_address);
    }

  return TRUE;
}

static RpmOstreeTransactionDeployFlags
deploy_flags_from_options (GVariant *options,
                           RpmOstreeTransactionDeployFlags defaults)
{
  RpmOstreeTransactionDeployFlags ret = defaults;
  GVariantDict options_dict;
  gboolean opt_reboot = FALSE;
  gboolean opt_skip_purge = FALSE;
  gboolean opt_allow_downgrade = FALSE;
  gboolean opt_dry_run = FALSE;

  g_variant_dict_init (&options_dict, options);

  g_variant_dict_lookup (&options_dict,
                         "allow-downgrade", "b",
                         &opt_allow_downgrade);
  if (opt_allow_downgrade)
    ret |= RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE;
  g_variant_dict_lookup (&options_dict,
                         "reboot", "b",
                         &opt_reboot);
  if (opt_reboot)
    ret |= RPMOSTREE_TRANSACTION_DEPLOY_FLAG_REBOOT;
  g_variant_dict_lookup (&options_dict,
                         "skip-purge", "b",
                         &opt_skip_purge);
  if (opt_skip_purge)
    ret |= RPMOSTREE_TRANSACTION_DEPLOY_FLAG_SKIP_PURGE;

  g_variant_dict_lookup (&options_dict, "dry-run", "b",
                         &opt_dry_run);
  if (opt_dry_run)
    ret |= RPMOSTREE_TRANSACTION_DEPLOY_FLAG_DRY_RUN;

  g_variant_dict_clear (&options_dict);

  return ret;
}

static gboolean
os_handle_deploy (RPMOSTreeOS *interface,
                  GDBusMethodInvocation *invocation,
                  GUnixFDList *fdlist,
                  const char *arg_revision,
                  GVariant *arg_options)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_deploy (invocation,
                                                   ot_sysroot,
                                                   deploy_flags_from_options (arg_options,
                                                      RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE),
                                                   osname, NULL, arg_revision, NULL, NULL, NULL,
                                                   cancellable,
                                                   &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_deploy (interface, invocation, NULL, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_upgrade (RPMOSTreeOS *interface,
                   GDBusMethodInvocation *invocation,
                   GUnixFDList *fdlist,
                   GVariant *arg_options)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_deploy (invocation, ot_sysroot,
                                                   deploy_flags_from_options (arg_options, 0),
                                                   osname, NULL, NULL, NULL, NULL, NULL,
                                                   cancellable, &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_upgrade (interface, invocation, NULL, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_rollback (RPMOSTreeOS *interface,
                    GDBusMethodInvocation *invocation,
                    GVariant *arg_options)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  gboolean opt_reboot = FALSE;
  GVariantDict options_dict;
  GError *local_error = NULL;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  g_variant_dict_init (&options_dict, arg_options);

  g_variant_dict_lookup (&options_dict,
                         "reboot", "b",
                         &opt_reboot);

  g_variant_dict_clear (&options_dict);

  transaction = rpmostreed_transaction_new_rollback (invocation,
                                                     ot_sysroot,
                                                     osname,
                                                     opt_reboot,
                                                     cancellable,
                                                     &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_rollback (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_clear_rollback_target (RPMOSTreeOS *interface,
                                 GDBusMethodInvocation *invocation,
                                 GVariant *arg_options)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  gboolean opt_reboot = FALSE;
  GVariantDict options_dict;
  GError *local_error = NULL;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  g_variant_dict_init (&options_dict, arg_options);

  g_variant_dict_lookup (&options_dict,
                         "reboot", "b",
                         &opt_reboot);

  g_variant_dict_clear (&options_dict);

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_clear_rollback (invocation,
                                                           ot_sysroot,
                                                           osname,
                                                           opt_reboot,
                                                           cancellable,
                                                           &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_clear_rollback_target (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_rebase (RPMOSTreeOS *interface,
                  GDBusMethodInvocation *invocation,
                  GUnixFDList *fdlist,
                  GVariant *arg_options,
                  const char *arg_refspec,
                  const char * const *arg_packages)
{
  /* TODO: Totally ignoring arg_packages for now */
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  GVariantDict options_dict;
  const char *osname;
  const char *opt_revision = NULL;
  GError *local_error = NULL;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  g_variant_dict_init (&options_dict, arg_options);
  g_variant_dict_lookup (&options_dict,
                         "revision", "&s",
                         &opt_revision);
  g_variant_dict_clear (&options_dict);

  transaction = rpmostreed_transaction_new_deploy (invocation,
                                                   ot_sysroot,
                                                   deploy_flags_from_options (arg_options,
                                                      RPMOSTREE_TRANSACTION_DEPLOY_FLAG_ALLOW_DOWNGRADE),
                                                   osname, arg_refspec, opt_revision,
                                                   NULL, NULL, NULL,
                                                   cancellable,
                                                   &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_rebase (interface, invocation, NULL, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_pkg_change (RPMOSTreeOS *interface,
                      GDBusMethodInvocation *invocation,
                      GUnixFDList *fdlist,
                      GVariant *arg_options,
                      const char * const *arg_packages_added,
                      const char * const *arg_packages_removed)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;
  g_auto(GVariantDict) options_dict;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  /* Right now, we only use the fd list from the D-Bus message to transfer local
   * packages, so there's no point in re-extracting them from fdlist based on
   * the indices in local-packages. We just do a sanity check here that they are
   * indeed the same length. */
  gsize n_local_pkgs = 0;
  g_variant_dict_init (&options_dict, arg_options);
  if (g_variant_dict_contains (&options_dict, "install-local-packages"))
    {
      g_autoptr(GVariant) fds =
        g_variant_dict_lookup_value (&options_dict, "install-local-packages",
                                     G_VARIANT_TYPE ("ah"));
      g_assert (fds);
      n_local_pkgs = g_variant_n_children (fds);
    }

  if (g_unix_fd_list_get_length (fdlist) != n_local_pkgs)
    {
        g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Expected %lu fds but received %d", n_local_pkgs,
                     g_unix_fd_list_get_length (fdlist));
        goto out;
    }

  /* By default, pkgchange does not pull the base; we will likley
   * add an option in the future.
   */
  transaction = rpmostreed_transaction_new_deploy (invocation,
                                                   ot_sysroot,
                                                   deploy_flags_from_options (arg_options,
                                                     RPMOSTREE_TRANSACTION_DEPLOY_FLAG_NO_PULL_BASE),
                                                   osname, NULL, NULL,
                                                   arg_packages_added,
                                                   arg_packages_removed, fdlist,
                                                   cancellable, &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_pkg_change (interface, invocation, NULL, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_set_initramfs_state (RPMOSTreeOS *interface,
                               GDBusMethodInvocation *invocation,
                               gboolean regenerate,
                               const char *const*args,
                               GVariant *arg_options)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(GVariantDict) dict = NULL;
  const char *osname;
  gboolean reboot = FALSE;
  GError *local_error = NULL;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  dict = g_variant_dict_new (arg_options);
  g_variant_dict_lookup (dict, "reboot", "b", &reboot);

  transaction = rpmostreed_transaction_new_initramfs_state (invocation,
                                                            ot_sysroot,
                                                            osname,
                                                            regenerate,
                                                            (char**)args,
                                                            reboot,
                                                            cancellable,
                                                            &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_pkg_change (interface, invocation, NULL, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_cleanup (RPMOSTreeOS *interface,
                   GDBusMethodInvocation *invocation,
                   const char *const*args)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  RpmOstreeTransactionCleanupFlags flags = 0;
  const char *osname;
  GError *local_error = NULL;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  for (char **iter = (char**) args; iter && *iter; iter++)
    {
      const char *v = *iter;
      if (strcmp (v, "base") == 0)
        flags |= RPMOSTREE_TRANSACTION_CLEANUP_BASE;
      else if (strcmp (v, "pending-deploy") == 0)
        flags |= RPMOSTREE_TRANSACTION_CLEANUP_PENDING_DEPLOY;
      else if (strcmp (v, "rollback-deploy") == 0)
        flags |= RPMOSTREE_TRANSACTION_CLEANUP_ROLLBACK_DEPLOY;
      else if (strcmp (v, "repomd") == 0)
        flags |= RPMOSTREE_TRANSACTION_CLEANUP_REPOMD;
      else
        {
          g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid cleanup type: %s", v);
          goto out;
        }
    }


  transaction = rpmostreed_transaction_new_cleanup (invocation,
                                                    ot_sysroot,
                                                    osname,
                                                    flags,
                                                    cancellable,
                                                    &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_pkg_change (interface, invocation, NULL, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_get_cached_rebase_rpm_diff (RPMOSTreeOS *interface,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_refspec,
                                      const char * const *arg_packages)
{
  RpmostreedSysroot *global_sysroot;
  g_autoptr(GCancellable) cancellable = NULL;
  OstreeSysroot *ot_sysroot = NULL;
  OstreeRepo *ot_repo = NULL;
  const gchar *name;
  glnx_unref_object OstreeDeployment *base_deployment = NULL;
  g_autoptr(RpmOstreeOrigin) origin = NULL;
  g_autofree gchar *comp_ref = NULL;
  GError *local_error = NULL;
  GVariant *value = NULL; /* freed when invoked */
  GVariant *details = NULL; /* freed when invoked */

  /* TODO: Totally ignoring packages for now */

  global_sysroot = rpmostreed_sysroot_get ();

  ot_sysroot = rpmostreed_sysroot_get_root (global_sysroot);
  ot_repo = rpmostreed_sysroot_get_repo (global_sysroot);

  name = rpmostree_os_get_name (interface);
  base_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
  if (base_deployment == NULL)
    {
      local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "No deployments found for os %s", name);
      goto out;
    }

  origin = rpmostree_origin_parse_deployment (base_deployment, &local_error);
  if (!origin)
    goto out;

  if (!rpmostreed_refspec_parse_partial (arg_refspec,
                                         rpmostree_origin_get_refspec (origin),
                                         &comp_ref,
                                         &local_error))
    goto out;

  value = rpm_ostree_db_diff_variant (ot_repo,
                                      ostree_deployment_get_csum (base_deployment),
                                      comp_ref,
                                      cancellable,
                                      &local_error);
  if (value == NULL)
    goto out;

  details = rpmostreed_commit_generate_cached_details_variant (base_deployment,
                                                               ot_repo,
                                                               comp_ref,
							       &local_error);
  if (!details)
    goto out;

out:
  if (local_error == NULL)
    {
      g_dbus_method_invocation_return_value (invocation, new_variant_diff_result (value, details));
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
os_handle_download_rebase_rpm_diff (RPMOSTreeOS *interface,
                                    GDBusMethodInvocation *invocation,
                                    const char *arg_refspec,
                                    const char * const *arg_packages)
{
  /* TODO: Totally ignoring arg_packages for now */
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_package_diff (invocation,
                                                         ot_sysroot,
                                                         osname,
                                                         arg_refspec,
                                                         NULL,  /* revision */
                                                         cancellable,
                                                         &local_error);

  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_download_rebase_rpm_diff (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
os_handle_get_cached_deploy_rpm_diff (RPMOSTreeOS *interface,
                                      GDBusMethodInvocation *invocation,
                                      const char *arg_revision,
                                      const char * const *arg_packages)
{
  const char *base_checksum;
  const char *osname;
  OstreeSysroot *ot_sysroot = NULL;
  OstreeRepo *ot_repo = NULL;
  glnx_unref_object OstreeDeployment *base_deployment = NULL;
  g_autoptr(RpmOstreeOrigin) origin = NULL;
  g_autofree char *checksum = NULL;
  g_autofree char *version = NULL;
  g_autoptr(GCancellable) cancellable = NULL;
  GVariant *value = NULL;
  GVariant *details = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;

  /* XXX Ignoring arg_packages for now. */

  ot_sysroot = rpmostreed_sysroot_get_root (rpmostreed_sysroot_get ());
  ot_repo = rpmostreed_sysroot_get_repo (rpmostreed_sysroot_get ());

  osname = rpmostree_os_get_name (interface);
  base_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, osname);
  if (base_deployment == NULL)
    {
      local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "No deployments found for os %s", osname);
      goto out;
    }

  origin = rpmostree_origin_parse_deployment (base_deployment, error);
  if (!origin)
    goto out;
 
  base_checksum = ostree_deployment_get_csum (base_deployment);

  if (!rpmostreed_parse_revision (arg_revision,
                                  &checksum,
                                  &version,
                                  &local_error))
    goto out;

  if (version != NULL)
    {
      if (!rpmostreed_repo_lookup_cached_version (ot_repo,
                                                  rpmostree_origin_get_refspec (origin),
                                                  version,
                                                  cancellable,
                                                  &checksum,
                                                  &local_error))
        goto out;
    }

  value = rpm_ostree_db_diff_variant (ot_repo,
                                      base_checksum,
                                      checksum,
                                      cancellable,
                                      &local_error);
  if (value == NULL)
    goto out;

  details = rpmostreed_commit_generate_cached_details_variant (base_deployment,
                                                               ot_repo,
                                                               NULL,
							       &local_error);
  if (!details)
    goto out;

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      g_dbus_method_invocation_return_value (invocation,
					     new_variant_diff_result (value, details));
    }

  return TRUE;
}

static gboolean
os_handle_download_deploy_rpm_diff (RPMOSTreeOS *interface,
                                    GDBusMethodInvocation *invocation,
                                    const char *arg_revision,
                                    const char * const *arg_packages)
{
  RpmostreedOS *self = RPMOSTREED_OS (interface);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  const char *osname;
  GError *local_error = NULL;

  /* XXX Ignoring arg_packages for now. */

  transaction = merge_compatible_txn (self, invocation);
  if (transaction)
    goto out;

  if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (),
                                      cancellable,
                                      &ot_sysroot,
                                      NULL,
                                      &local_error))
    goto out;

  osname = rpmostree_os_get_name (interface);

  transaction = rpmostreed_transaction_new_package_diff (invocation,
                                                         ot_sysroot,
                                                         osname,
                                                         NULL, /* refspec */
                                                         arg_revision,
                                                         cancellable,
                                                         &local_error);
  if (transaction == NULL)
    goto out;

  rpmostreed_transaction_monitor_add (self->transaction_monitor, transaction);

out:
  if (local_error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }
  else
    {
      const char *client_address;
      client_address = rpmostreed_transaction_get_client_address (transaction);
      rpmostree_os_complete_download_deploy_rpm_diff (interface, invocation, client_address);
    }

  return TRUE;
}

static gboolean
rpmostreed_os_load_internals (RpmostreedOS *self, GError **error)
{
  const gchar *name;

  OstreeDeployment *booted = NULL; /* owned by sysroot */
  g_autofree gchar* booted_id = NULL;
  glnx_unref_object  OstreeDeployment *merge_deployment = NULL; /* transfered */

  g_autoptr(GPtrArray) deployments = NULL;
  OstreeSysroot *ot_sysroot;
  OstreeRepo *ot_repo;
  GVariant *booted_variant = NULL;
  GVariant *default_variant = NULL;
  GVariant *rollback_variant = NULL;
  GVariant *cached_update = NULL;
  gboolean has_cached_updates = FALSE;

  gint rollback_index;
  guint i;

  name = rpmostree_os_get_name (RPMOSTREE_OS (self));
  g_debug ("loading %s", name);

  ot_sysroot = rpmostreed_sysroot_get_root (rpmostreed_sysroot_get ());
  ot_repo = rpmostreed_sysroot_get_repo (rpmostreed_sysroot_get ());

  booted = ostree_sysroot_get_booted_deployment (ot_sysroot);
  if (booted && g_strcmp0 (ostree_deployment_get_osname (booted), name) == 0)
    {
      booted_variant = rpmostreed_deployment_generate_variant (booted, booted_id, ot_repo, error);
      if (!booted_variant)
        return FALSE;
      booted_id = rpmostreed_deployment_generate_id (booted);
    }

  deployments = ostree_sysroot_get_deployments (ot_sysroot);
  for (i=0; i<deployments->len; i++)
    {
      if (g_strcmp0 (ostree_deployment_get_osname (deployments->pdata[i]), name) == 0)
        {
          default_variant = rpmostreed_deployment_generate_variant (deployments->pdata[i],
                                                                    booted_id,
                                                                    ot_repo, error);
	  if (default_variant == NULL)
	    return FALSE;
          break;
        }
    }

  if (deployments->len >= 2)
    {
      rollback_index = rpmostreed_rollback_deployment_index (name, ot_sysroot, NULL);
      if (rollback_index >= 0)
	{
	  rollback_variant = rpmostreed_deployment_generate_variant (deployments->pdata[rollback_index], booted_id,
								     ot_repo, error);
	  if (!rollback_variant)
	    return FALSE;
	}
    }

  merge_deployment = ostree_sysroot_get_merge_deployment (ot_sysroot, name);
  if (merge_deployment)
    {
      g_autoptr(RpmOstreeOrigin) origin = NULL;

      /* Don't fail here for unknown origin types */
      origin = rpmostree_origin_parse_deployment (merge_deployment, NULL);
      if (origin)
        {
          cached_update = rpmostreed_commit_generate_cached_details_variant (merge_deployment,
                                                                             ot_repo,
                                                                             rpmostree_origin_get_refspec (origin),
                                                                             error);
          if (!cached_update)
            return FALSE;
          has_cached_updates = cached_update != NULL;
        }
   }

  if (!booted_variant)
    booted_variant = rpmostreed_deployment_generate_blank_variant ();
  rpmostree_os_set_booted_deployment (RPMOSTREE_OS (self),
                                      booted_variant);

  if (!default_variant)
    default_variant = rpmostreed_deployment_generate_blank_variant ();
  rpmostree_os_set_default_deployment (RPMOSTREE_OS (self),
                                       default_variant);

  if (!rollback_variant)
    rollback_variant = rpmostreed_deployment_generate_blank_variant ();
  rpmostree_os_set_rollback_deployment (RPMOSTREE_OS (self),
                                        rollback_variant);

  rpmostree_os_set_cached_update (RPMOSTREE_OS (self), cached_update);
  rpmostree_os_set_has_cached_update_rpm_diff (RPMOSTREE_OS (self),
                                               has_cached_updates);
  g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON (self));

  return TRUE;
}

static void
rpmostreed_os_iface_init (RPMOSTreeOSIface *iface)
{
  iface->handle_get_cached_update_rpm_diff = os_handle_get_cached_update_rpm_diff;
  iface->handle_get_deployments_rpm_diff   = os_handle_get_deployments_rpm_diff;
  iface->handle_download_update_rpm_diff   = os_handle_download_update_rpm_diff;
  iface->handle_deploy                     = os_handle_deploy;
  iface->handle_upgrade                    = os_handle_upgrade;
  iface->handle_rollback                   = os_handle_rollback;
  iface->handle_clear_rollback_target      = os_handle_clear_rollback_target;
  iface->handle_rebase                     = os_handle_rebase;
  iface->handle_pkg_change                 = os_handle_pkg_change;
  iface->handle_set_initramfs_state        = os_handle_set_initramfs_state;
  iface->handle_cleanup                    = os_handle_cleanup;
  iface->handle_get_cached_rebase_rpm_diff = os_handle_get_cached_rebase_rpm_diff;
  iface->handle_download_rebase_rpm_diff   = os_handle_download_rebase_rpm_diff;
  iface->handle_get_cached_deploy_rpm_diff = os_handle_get_cached_deploy_rpm_diff;
  iface->handle_download_deploy_rpm_diff   = os_handle_download_deploy_rpm_diff;
}

/* ---------------------------------------------------------------------------------------------------- */

RPMOSTreeOS *
rpmostreed_os_new (OstreeSysroot *sysroot,
                   OstreeRepo *repo,
                   const char *name,
                   RpmostreedTransactionMonitor *monitor)
{
  RpmostreedOS *obj = NULL;
  g_autofree char *path = NULL;

  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (name != NULL, NULL);
  g_return_val_if_fail (RPMOSTREED_IS_TRANSACTION_MONITOR (monitor), NULL);

  path = rpmostreed_generate_object_path (BASE_DBUS_PATH, name, NULL);

  obj = g_object_new (RPMOSTREED_TYPE_OS, "name", name, NULL);

  /* FIXME Make this a construct-only property? */
  obj->transaction_monitor = g_object_ref (monitor);

  /* FIXME - use GInitable */
  { g_autoptr(GError) local_error = NULL;
    if (!rpmostreed_os_load_internals (obj, &local_error))
      g_warning ("%s", local_error->message);
  }
  
  rpmostreed_daemon_publish (rpmostreed_daemon_get (), path, FALSE, obj);

  return RPMOSTREE_OS (obj);
}
