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

#include "rpmostreed-daemon.h"
#include "rpmostreed-sysroot.h"
#include "rpmostreed-os.h"
#include "rpmostreed-os-experimental.h"
#include "rpmostree-util.h"
#include "rpmostreed-utils.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-errors.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-transaction-monitor.h"

#include "rpmostree-output.h"

#include <err.h>
#include "libglnx.h"
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <systemd/sd-login.h>
#include <systemd/sd-journal.h>

/**
 * SECTION: sysroot
 * @title: RpmostreedSysroot
 * @short_description: Implementation of #RPMOSTreeSysroot
 *
 * This type provides an implementation of the #RPMOSTreeSysroot interface.
 */

typedef struct _RpmostreedSysrootClass RpmostreedSysrootClass;

/**
 * RpmostreedSysroot:
 *
 * The #RpmostreedSysroot structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _RpmostreedSysroot {
  RPMOSTreeSysrootSkeleton parent_instance;

  OstreeSysroot *ot_sysroot;
  OstreeRepo *repo;
  struct stat repo_last_stat;
  RpmostreedTransactionMonitor *transaction_monitor;
  PolkitAuthority *authority;
  gboolean on_session_bus;

  GHashTable *os_interfaces;
  GHashTable *osexperimental_interfaces;

  /* The OS interface's various diff methods can run concurrently with
   * transactions, which is safe except when the transaction is writing
   * new deployments to disk or downloading RPM package details.  The
   * writer lock protects these critical sections so the diff methods
   * (holding reader locks) can run safely. */
  GRWLock method_rw_lock;

  GFileMonitor *monitor;
  guint sig_changed;
};

struct _RpmostreedSysrootClass {
  RPMOSTreeSysrootSkeletonClass parent_class;
};

enum {
  UPDATED,
  NUM_SIGNALS
};

static guint signals[NUM_SIGNALS];
static void rpmostreed_sysroot_iface_init (RPMOSTreeSysrootIface *iface);

G_DEFINE_TYPE_WITH_CODE (RpmostreedSysroot,
                         rpmostreed_sysroot,
                         RPMOSTREE_TYPE_SYSROOT_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_SYSROOT,
                                                rpmostreed_sysroot_iface_init));

static RpmostreedSysroot *_sysroot_instance;

/* ---------------------------------------------------------------------------------------------------- */

static void
sysroot_output_cb (RpmOstreeOutputType type, void *data, void *opaque)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (opaque);
  glnx_unref_object RpmostreedTransaction *transaction = NULL;

  transaction =
    rpmostreed_transaction_monitor_ref_active_transaction (self->transaction_monitor);

  if (!transaction)
    {
      rpmostree_output_default_handler (type, data, opaque);
      return;
    }

  switch (type)
  {
  case RPMOSTREE_OUTPUT_MESSAGE:
    rpmostree_transaction_emit_message (RPMOSTREE_TRANSACTION (transaction),
                                        ((RpmOstreeOutputMessage*)data)->text);
    break;
  case RPMOSTREE_OUTPUT_TASK_BEGIN:
    rpmostree_transaction_emit_task_begin (RPMOSTREE_TRANSACTION (transaction),
                                           ((RpmOstreeOutputTaskBegin*)data)->text);
    break;
  case RPMOSTREE_OUTPUT_TASK_END:
    rpmostree_transaction_emit_task_end (RPMOSTREE_TRANSACTION (transaction),
                                         ((RpmOstreeOutputTaskEnd*)data)->text);
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_PERCENT:
    rpmostree_transaction_emit_percent_progress (RPMOSTREE_TRANSACTION (transaction),
                                                 ((RpmOstreeOutputProgressPercent*)data)->text,
                                                 ((RpmOstreeOutputProgressPercent*)data)->percentage);
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_N_ITEMS:
    {
      RpmOstreeOutputProgressNItems *nitems = data;
      /* We still emit PercentProgress for compatibility with older clients as
       * well as Cockpit. It's not worth trying to deal with version skew just
       * for this yet.
       */
      int percentage = (nitems->current == nitems->total) ? 100 :
        (((double)(nitems->current)) / (nitems->total) * 100);
      g_autofree char *newtext = g_strdup_printf ("%s (%u/%u)", nitems->text, nitems->current, nitems->total);
      rpmostree_transaction_emit_percent_progress (RPMOSTREE_TRANSACTION (transaction), newtext, percentage);
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_END:
    rpmostree_transaction_emit_progress_end (RPMOSTREE_TRANSACTION (transaction));
    break;
  }
}

static gboolean
handle_create_osname (RPMOSTreeSysroot *object,
                      GDBusMethodInvocation *invocation,
                      const gchar *osname)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  GError *error = NULL;
  g_autofree gchar *dbus_path = NULL;

  if (!ostree_sysroot_ensure_initialized (self->ot_sysroot, NULL, &error))
    goto out;

  if (strchr (osname, '/') != 0)
    {
      g_set_error_literal (&error,
                           RPM_OSTREED_ERROR,
                           RPM_OSTREED_ERROR_FAILED,
                           "Invalid osname");
      goto out;
    }

  if (!ostree_sysroot_init_osname (self->ot_sysroot, osname, NULL, &error))
    goto out;

  dbus_path = rpmostreed_generate_object_path (BASE_DBUS_PATH, osname, NULL);

  rpmostree_sysroot_complete_create_osname (RPMOSTREE_SYSROOT (self),
                                            invocation,
                                            g_strdup (dbus_path));
out:
  if (error)
    g_dbus_method_invocation_take_error (invocation, error);

  return TRUE;
}

static gboolean
handle_get_os (RPMOSTreeSysroot *object,
               GDBusMethodInvocation *invocation,
               const char *arg_name)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);

  if (arg_name[0] == '\0')
    {
      rpmostree_sysroot_complete_get_os (object,
                                         invocation,
                                         rpmostree_sysroot_dup_booted (object));
      return FALSE;
    }

  g_autoptr(GDBusInterfaceSkeleton) os_interface =
    g_hash_table_lookup (self->os_interfaces, arg_name);
  if (os_interface != NULL)
    g_object_ref (os_interface);

  if (os_interface != NULL)
    {
      const char *object_path = g_dbus_interface_skeleton_get_object_path (os_interface);
      rpmostree_sysroot_complete_get_os (object, invocation, object_path);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_FOUND,
                                             "OS name \"%s\" not found",
                                             arg_name);
    }

  return TRUE;
}

static gboolean
sysroot_transform_transaction_to_attrs (GBinding *binding,
                                        const GValue *src_value,
                                        GValue *dst_value,
                                        gpointer user_data)
{
  RpmostreedTransaction *transaction;
  GVariant *variant;
  const char *method_name = "";
  const char *path = "";
  const char *sender_name = "";

  transaction = g_value_get_object (src_value);

  if (transaction != NULL)
    {
      GDBusMethodInvocation *invocation;

      invocation = rpmostreed_transaction_get_invocation (transaction);
      method_name = g_dbus_method_invocation_get_method_name (invocation);
      path = g_dbus_method_invocation_get_object_path (invocation);
      sender_name = g_dbus_method_invocation_get_sender (invocation);
    }

  variant = g_variant_new ("(sss)", method_name, sender_name, path);

  g_value_set_variant (dst_value, variant);

  return TRUE;
}

static gboolean
sysroot_transform_transaction_to_address (GBinding *binding,
                                          const GValue *src_value,
                                          GValue *dst_value,
                                          gpointer user_data)
{
  RpmostreedTransaction *transaction = g_value_get_object (src_value);
  const char *address = "";
  if (transaction != NULL)
    address = rpmostreed_transaction_get_client_address (transaction);
  g_value_set_string (dst_value, address);
  return TRUE;
}

static gboolean
sysroot_populate_deployments_unlocked (RpmostreedSysroot *self,
                                       gboolean *out_changed,
                                       GError **error)
{
  /* just set the out var early */
  if (out_changed)
    *out_changed = FALSE;

  gboolean sysroot_changed;
  if (!ostree_sysroot_load_if_changed (self->ot_sysroot, &sysroot_changed, NULL, error))
    return FALSE;

  struct stat repo_new_stat;
  if (!glnx_fstat (ostree_repo_get_dfd (self->repo), &repo_new_stat, error))
    return FALSE;

  const gboolean repo_changed =
    !((self->repo_last_stat.st_mtim.tv_sec  == repo_new_stat.st_mtim.tv_sec) &&
      (self->repo_last_stat.st_mtim.tv_nsec == repo_new_stat.st_mtim.tv_nsec));
  if (repo_changed)
    self->repo_last_stat = repo_new_stat;

  if (!(sysroot_changed || repo_changed))
    return TRUE; /* Note early return */

  g_debug ("loading deployments");

  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));

  g_autoptr(GHashTable) seen_osnames =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);

  /* Updated booted property; object owned by sysroot */
  g_autofree gchar *booted_id = NULL;
  OstreeDeployment *booted = ostree_sysroot_get_booted_deployment (self->ot_sysroot);
  if (booted)
    {
      const gchar *os = ostree_deployment_get_osname (booted);
      g_autofree gchar *path = rpmostreed_generate_object_path (BASE_DBUS_PATH, os, NULL);
      rpmostree_sysroot_set_booted (RPMOSTREE_SYSROOT (self), path);
      booted_id = rpmostreed_deployment_generate_id (booted);
    }
  else
    {
      rpmostree_sysroot_set_booted (RPMOSTREE_SYSROOT (self), "/");
    }

  /* Add deployment interfaces */
  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (self->ot_sysroot);

  for (guint i = 0; deployments != NULL && i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      GVariant *variant =
        rpmostreed_deployment_generate_variant (self->ot_sysroot, deployment,
                                                booted_id, self->repo, error);
      if (!variant)
        return glnx_prefix_error (error, "Reading deployment %u", i);

      g_variant_builder_add_value (&builder, variant);

      const char *deployment_os = ostree_deployment_get_osname (deployment);

      /* Have we not seen this osname instance before?  If so, add it
       * now.
       */
      if (!g_hash_table_contains (self->os_interfaces, deployment_os))
        {
          RPMOSTreeOS *obj = rpmostreed_os_new (self->ot_sysroot, self->repo,
                                                deployment_os,
                                                self->transaction_monitor);
          g_hash_table_insert (self->os_interfaces, g_strdup (deployment_os), obj);

          RPMOSTreeOSExperimental *eobj = rpmostreed_osexperimental_new (self->ot_sysroot, self->repo,
                                                                         deployment_os,
                                                                         self->transaction_monitor);
          g_hash_table_insert (self->osexperimental_interfaces, g_strdup (deployment_os), eobj);

        }
      /* Owned by deployment, hash lifetime is smaller */
      g_hash_table_add (seen_osnames, (char*)deployment_os);
    }

  /* Remove dead os paths */
  GLNX_HASH_TABLE_FOREACH_IT (self->os_interfaces, it, const char*, k, GObject*, v)
    {
      if (!g_hash_table_contains (seen_osnames, k))
        {
          g_object_run_dispose (G_OBJECT (v));
          g_hash_table_iter_remove (&it);

          g_hash_table_remove (self->osexperimental_interfaces, k);
        }
    }

  rpmostree_sysroot_set_deployments (RPMOSTREE_SYSROOT (self),
                                     g_variant_builder_end (&builder));
  g_debug ("finished deployments");

  if (out_changed)
    *out_changed = TRUE;
  return TRUE;
}

static gboolean
handle_register_client (RPMOSTreeSysroot *object,
                        GDBusMethodInvocation *invocation,
                        GVariant *arg_options)
{
  const char *sender;
  sender = g_dbus_method_invocation_get_sender (invocation);
  g_assert (sender);

  rpmostreed_daemon_add_client (rpmostreed_daemon_get (), sender);
  rpmostree_sysroot_complete_register_client (object, invocation);

  return TRUE;
}

static gboolean
handle_unregister_client (RPMOSTreeSysroot *object,
                          GDBusMethodInvocation *invocation,
                          GVariant *arg_options)
{
  const char *sender;

  sender = g_dbus_method_invocation_get_sender (invocation);
  g_assert (sender);

  rpmostreed_daemon_remove_client (rpmostreed_daemon_get (), sender);
  rpmostree_sysroot_complete_unregister_client (object, invocation);

  return TRUE;
}

static gboolean
handle_reload_config (RPMOSTreeSysroot *object,
                      GDBusMethodInvocation *invocation)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

  if (!rpmostreed_sysroot_reload (self, error))
    goto out;

  rpmostree_sysroot_complete_reload_config (object, invocation);
out:
  if (local_error)
    g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&local_error));

  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */
static void
sysroot_dispose (GObject *object)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  gint tries;

  if (self->monitor)
    {
      if (self->sig_changed)
        g_signal_handler_disconnect (self->monitor, self->sig_changed);
      self->sig_changed = 0;

      /* HACK - It is not generally safe to just unref a GFileMonitor.
       * Some events might be on their way to the main loop from its
       * worker thread and if they arrive after the GFileMonitor has
       * been destroyed, bad things will happen.
       *
       * As a workaround, we cancel the monitor and then spin the main
       * loop a bit until nothing is pending anymore.
       *
       * https://bugzilla.gnome.org/show_bug.cgi?id=740491
       */

      g_file_monitor_cancel (self->monitor);
      for (tries = 0; tries < 10; tries ++)
        {
          if (!g_main_context_iteration (NULL, FALSE))
            break;
        }
    }

  /* Tracked os paths are responsible to unpublish themselves */
  GLNX_HASH_TABLE_FOREACH_KV (self->os_interfaces, const char*, k, GObject*, value)
    g_object_run_dispose (value);
  g_hash_table_remove_all (self->os_interfaces);
  g_hash_table_remove_all (self->osexperimental_interfaces);

  g_clear_object (&self->transaction_monitor);
  g_clear_object (&self->authority);

  G_OBJECT_CLASS (rpmostreed_sysroot_parent_class)->dispose (object);
}

static void
sysroot_finalize (GObject *object)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  _sysroot_instance = NULL;

  g_hash_table_unref (self->os_interfaces);
  g_hash_table_unref (self->osexperimental_interfaces);

  g_clear_object (&self->monitor);

  rpmostree_output_set_callback (NULL, NULL);

  G_OBJECT_CLASS (rpmostreed_sysroot_parent_class)->finalize (object);
}

static void
rpmostreed_sysroot_init (RpmostreedSysroot *self)
{
  g_assert (_sysroot_instance == NULL);
  _sysroot_instance = self;

  self->os_interfaces = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify) g_object_unref);
  self->osexperimental_interfaces = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                           (GDestroyNotify) g_object_unref);

  self->monitor = NULL;

  self->transaction_monitor = rpmostreed_transaction_monitor_new ();

  if (g_getenv ("RPMOSTREE_USE_SESSION_BUS") != NULL)
    self->on_session_bus = TRUE;

  /* Only use polkit when running as root on system bus; self-tests don't need it */
  if (!self->on_session_bus)
    {
      g_autoptr(GError) local_error = NULL;
      self->authority = polkit_authority_get_sync (NULL, &local_error);
      if (self->authority == NULL)
        {
          errx (EXIT_FAILURE, "Can't get polkit authority: %s", local_error->message);
        }
    }

  rpmostree_output_set_callback (sysroot_output_cb, self);
}

static void
sysroot_constructed (GObject *object)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);

  g_object_bind_property_full (self->transaction_monitor,
                               "active-transaction",
                               self,
                               "active-transaction",
                               G_BINDING_DEFAULT |
                               G_BINDING_SYNC_CREATE,
                               sysroot_transform_transaction_to_attrs,
                               NULL,
                               NULL,
                               NULL);
  g_object_bind_property_full (self->transaction_monitor,
                               "active-transaction",
                               self,
                               "active-transaction-path",
                               G_BINDING_DEFAULT |
                               G_BINDING_SYNC_CREATE,
                               sysroot_transform_transaction_to_address,
                               NULL,
                               NULL,
                               NULL);

  G_OBJECT_CLASS (rpmostreed_sysroot_parent_class)->constructed (object);
}

static gboolean
sysroot_authorize_method (GDBusInterfaceSkeleton *interface,
                          GDBusMethodInvocation  *invocation)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (interface);
  const gchar *method_name = g_dbus_method_invocation_get_method_name (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  gboolean authorized = FALSE;
  const char *action = NULL;

  if (self->on_session_bus)
    {
      /* The daemon is on the session bus, running self tests */
      authorized = TRUE;
    }
  else if (g_strcmp0 (method_name, "GetOS") == 0)
    {
      /* GetOS() is always allowed */
      authorized = TRUE;
    }
  else if (g_strcmp0 (method_name, "ReloadConfig") == 0)
    {
      action = "org.projectatomic.rpmostree1.reload-daemon";
    }
  else if (g_strcmp0 (method_name, "Cancel") == 0)
    {
      action = "org.projectatomic.rpmostree1.cancel";
    }
  else if (g_strcmp0 (method_name, "RegisterClient") == 0 ||
           g_strcmp0 (method_name, "UnregisterClient") == 0)
    {
      action = "org.projectatomic.rpmostree1.client-management";

      /* automatically allow register/unregister for users with active sessions */
      uid_t uid;
      if (rpmostreed_get_client_uid (rpmostreed_daemon_get (), sender, &uid))
        {
          g_autofree char *state = NULL;
          if (sd_uid_get_state (uid, &state) >= 0)
            {
              if (g_strcmp0 (state, "active") == 0)
                {
                  authorized = TRUE;
                  sd_journal_print (LOG_INFO, "Allowing active client %s (uid %d)",
                                    sender, uid);
                }
            }
          else
            {
              sd_journal_print (LOG_WARNING, "Failed to get state for uid %d", uid);
            }
        }
    }

  /* only ask polkit if we didn't already authorize it */
  if (!authorized && action != NULL)
    {
      glnx_unref_object PolkitSubject *subject = polkit_system_bus_name_new (sender);
      g_autoptr(GError) local_error = NULL;

      glnx_unref_object PolkitAuthorizationResult *result =
        polkit_authority_check_authorization_sync (self->authority, subject,
                                                   action, NULL,
                                                   POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                                   NULL, &local_error);
      if (result == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "Authorization error: %s",
                                                 local_error->message);
          return FALSE;
        }

      authorized = polkit_authorization_result_get_is_authorized (result);
    }

  if (!authorized)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "rpmostreed Sysroot operation %s not allowed for user",
                                             method_name);
    }

  return authorized;
}

static void
rpmostreed_sysroot_class_init (RpmostreedSysrootClass *klass)
{
  GObjectClass *gobject_class;
  GDBusInterfaceSkeletonClass *gdbus_interface_skeleton_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = sysroot_dispose;
  gobject_class->finalize     = sysroot_finalize;
  gobject_class->constructed  = sysroot_constructed;

  signals[UPDATED] = g_signal_new ("updated",
                                   RPMOSTREED_TYPE_SYSROOT,
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);

  gdbus_interface_skeleton_class = G_DBUS_INTERFACE_SKELETON_CLASS (klass);
  gdbus_interface_skeleton_class->g_authorize_method = sysroot_authorize_method;
}

gboolean
rpmostreed_sysroot_reload (RpmostreedSysroot *self,
                           GError **error)
{
  gboolean ret = FALSE;
  gboolean did_change;

  if (!sysroot_populate_deployments_unlocked (self, &did_change, error))
    goto out;
  if (!ostree_repo_reload_config (self->repo, NULL, error))
    goto out;

  ret = TRUE;
 out:
  if (ret && did_change)
    g_signal_emit (self, signals[UPDATED], 0);
  return ret;
}

static void
on_deploy_changed (GFileMonitor *monitor,
                   GFile *file,
                   GFile *other_file,
                   GFileMonitorEvent event_type,
                   gpointer user_data)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (user_data);
  g_autoptr(GError) error = NULL;

  if (event_type == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
    {
      if (!rpmostreed_sysroot_reload (self, &error))
        goto out;
    }

 out:
  if (error)
    g_critical ("Unable to update state: %s", error->message);
}

static void
rpmostreed_sysroot_iface_init (RPMOSTreeSysrootIface *iface)
{
  iface->handle_create_osname = handle_create_osname;
  iface->handle_get_os = handle_get_os;
  iface->handle_register_client = handle_register_client;
  iface->handle_unregister_client = handle_unregister_client;
  iface->handle_reload_config = handle_reload_config;
}

/**
 * rpmostreed_sysroot_populate :
 *
 * loads internals and starts monitoring
 *
 * Returns: True on success
 */
gboolean
rpmostreed_sysroot_populate (RpmostreedSysroot *self,
                             GCancellable *cancellable,
                             GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);

  const char *sysroot_path = rpmostree_sysroot_get_path (RPMOSTREE_SYSROOT (self));
  g_autoptr(GFile) sysroot_file = g_file_new_for_path (sysroot_path);
  self->ot_sysroot = ostree_sysroot_new (sysroot_file);

  /* This creates and caches an OstreeRepo instance inside
   * OstreeSysroot to ensure subsequent ostree_sysroot_get_repo()
   * calls won't fail.
   */
  if (!ostree_sysroot_get_repo (self->ot_sysroot, &self->repo, cancellable, error))
    return FALSE;

  if (!sysroot_populate_deployments_unlocked (self, NULL, error))
    return FALSE;

  if (self->monitor == NULL)
    {
      const char *sysroot_path = gs_file_get_path_cached (ostree_sysroot_get_path (self->ot_sysroot));
      g_autofree char *sysroot_deploy_path = g_build_filename (sysroot_path, "ostree/deploy", NULL);
      g_autoptr(GFile) sysroot_deploy = g_file_new_for_path (sysroot_deploy_path);

      self->monitor = g_file_monitor (sysroot_deploy, 0, NULL, error);

      if (self->monitor == NULL)
        return FALSE;

      self->sig_changed = g_signal_connect (self->monitor,
                                            "changed",
                                            G_CALLBACK (on_deploy_changed),
                                            self);
    }

  return TRUE;
}

gboolean
rpmostreed_sysroot_load_state (RpmostreedSysroot *self,
                               GCancellable *cancellable,
                               OstreeSysroot **out_sysroot,
                               OstreeRepo **out_repo,
                               GError **error)
{
  if (out_sysroot)
    *out_sysroot = g_object_ref (rpmostreed_sysroot_get_root (self));
  if (out_repo)
    *out_repo = g_object_ref (rpmostreed_sysroot_get_repo (self));
  return TRUE;
}

OstreeSysroot *
rpmostreed_sysroot_get_root (RpmostreedSysroot *self)
{
  return self->ot_sysroot;
}

OstreeRepo *
rpmostreed_sysroot_get_repo (RpmostreedSysroot *self)
{
  return self->repo;
}

PolkitAuthority *
rpmostreed_sysroot_get_polkit_authority (RpmostreedSysroot *self)
{
  return self->authority;
}

gboolean
rpmostreed_sysroot_is_on_session_bus (RpmostreedSysroot *self)
{
  return self->on_session_bus;
}

/**
 * rpmostreed_sysroot_get:
 *
 * Returns: (transfer none): The singleton #RpmostreedSysroot instance
 */
RpmostreedSysroot *
rpmostreed_sysroot_get (void)
{
  g_assert (_sysroot_instance);
  return _sysroot_instance;
}
