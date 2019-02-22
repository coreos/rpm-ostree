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

#include "rpmostree-output.h"

#include <err.h>
#include "libglnx.h"
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <systemd/sd-login.h>
#include <systemd/sd-journal.h>

/* Avoid clients leaking their bus connections keeping the transaction open */
#define FORCE_CLOSE_TXN_TIMEOUT_SECS 30

static gboolean
sysroot_reload_ostree_configs_and_deployments (RpmostreedSysroot *self,
                                               gboolean *out_changed,
                                               GError **error);

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
  RpmostreedTransaction *transaction;
  guint close_transaction_timeout_id;
  PolkitAuthority *authority;
  gboolean on_session_bus;

  GHashTable *os_interfaces;
  GHashTable *osexperimental_interfaces;

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
  gboolean output_to_self = FALSE;

  // The API previously passed these each time, but now we retain them as
  // statics.
  static char *progress_str;
  static bool progress_state_percent;
  static guint progress_state_n_items;

  if (self->transaction)
    g_object_get (self->transaction, "output-to-self", &output_to_self, NULL);

  if (!self->transaction || output_to_self)
    {
      rpmostree_output_default_handler (type, data, opaque);
      return;
    }

  RPMOSTreeTransaction *transaction = RPMOSTREE_TRANSACTION (self->transaction);
  switch (type)
  {
  case RPMOSTREE_OUTPUT_MESSAGE:
    rpmostree_transaction_emit_message (transaction, ((RpmOstreeOutputMessage*)data)->text);
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_BEGIN:
    {
      RpmOstreeOutputProgressBegin *begin = data;
      g_clear_pointer (&progress_str, g_free);
      progress_state_percent = false;
      progress_state_n_items = 0;
      if (begin->percent)
        {
          progress_str = g_strdup (begin->prefix);
          rpmostree_transaction_emit_percent_progress (transaction, progress_str, 0);
          progress_state_percent = true;
        }
      else if (begin->n > 0)
        {
          progress_str = g_strdup (begin->prefix);
          progress_state_n_items = begin->n;
          /* For backcompat, this is a percentage.  See below */
          rpmostree_transaction_emit_percent_progress (transaction, progress_str, 0);
        }
      else
        {
          rpmostree_transaction_emit_task_begin (transaction, begin->prefix);
        }
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_UPDATE:
    {
      RpmOstreeOutputProgressUpdate *update = data;
      if (progress_state_n_items)
        {
          /* We still emit PercentProgress for compatibility with older clients as
           * well as Cockpit. It's not worth trying to deal with version skew just
           * for this yet.
           */
          int percentage = (update->c == progress_state_n_items) ? 100 :
            (((double)(update->c)) / (progress_state_n_items) * 100);
          g_autofree char *newtext = g_strdup_printf ("%s (%u/%u)", progress_str, update->c, progress_state_n_items);
          rpmostree_transaction_emit_percent_progress (transaction, newtext, percentage);
        }
      else
        {
          rpmostree_transaction_emit_percent_progress (transaction, progress_str, update->c);
        }
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_SUB_MESSAGE:
    {
      /* Not handled right now */
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_END:
    {
      if (progress_state_percent || progress_state_n_items > 0)
        {
          rpmostree_transaction_emit_progress_end (transaction);
        }
      else
        {
          rpmostree_transaction_emit_task_end (transaction, "done");
        }
    }
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
                                                deployment_os);
          g_hash_table_insert (self->os_interfaces, g_strdup (deployment_os), obj);

          RPMOSTreeOSExperimental *eobj = rpmostreed_osexperimental_new (self->ot_sysroot, self->repo,
                                                                         deployment_os);
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
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_assert (sender);

  g_autoptr(GVariantDict) optdict = g_variant_dict_new (arg_options);
  const char *client_id = NULL;
  g_variant_dict_lookup (optdict, "id", "&s", &client_id);

  rpmostreed_daemon_add_client (rpmostreed_daemon_get (), sender, client_id);
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

/* remap relevant daemon configs to D-Bus properties */
static gboolean
reset_config_properties (RpmostreedSysroot  *self,
                         GError            **error)
{
  RpmostreedDaemon *daemon = rpmostreed_daemon_get ();

  RpmostreedAutomaticUpdatePolicy policy = rpmostreed_get_automatic_update_policy (daemon);
  const char *policy_str = rpmostree_auto_update_policy_to_str (policy, NULL);
  g_assert (policy_str);
  rpmostree_sysroot_set_automatic_update_policy (RPMOSTREE_SYSROOT (self), policy_str);

  return TRUE;
}

/* reloads *only* deployments and os internals, *no* configuration files */
static gboolean
handle_reload (RPMOSTreeSysroot *object,
               GDBusMethodInvocation *invocation)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  g_autoptr(GError) local_error = NULL;

  if (!sysroot_populate_deployments_unlocked (self, NULL, &local_error))
    goto out;

  /* always send an UPDATED signal to also force OS interfaces to reload */
  g_signal_emit (self, signals[UPDATED], 0);

  rpmostree_sysroot_complete_reload (object, invocation);

out:
  if (local_error)
    {
      g_prefix_error (&local_error, "Handling reload: ");
      g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&local_error));
    }
  return TRUE;
}

/* reloads *everything*: ostree configs, rpm-ostreed.conf, deployments, os internals */
static gboolean
handle_reload_config (RPMOSTreeSysroot *object,
                      GDBusMethodInvocation *invocation)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

  gboolean changed = FALSE;
  if (!rpmostreed_daemon_reload_config (rpmostreed_daemon_get (), &changed, error))
    goto out;

  if (changed && !reset_config_properties (self, error))
    goto out;

  gboolean sysroot_changed;
  if (!sysroot_reload_ostree_configs_and_deployments (self, &sysroot_changed, error))
    goto out;

  /* also send an UPDATED signal if configs changed to cause OS interfaces to reload; we do
   * it here if not done already in `rpmostreed_sysroot_reload` */
  if (changed && !sysroot_changed)
    g_signal_emit (self, signals[UPDATED], 0);

  rpmostree_sysroot_complete_reload_config (object, invocation);
out:
  if (local_error)
    {
      g_prefix_error (&local_error, "Handling config reload: ");
      g_dbus_method_invocation_take_error (invocation, g_steal_pointer (&local_error));
    }

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

  g_clear_object (&self->transaction);
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
  else if (g_strcmp0 (method_name, "GetOS") == 0 ||
           g_strcmp0 (method_name, "Reload") == 0)
    {
      /* GetOS() and Reload() are always allowed */
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

  signals[UPDATED] = g_signal_new ("updated",
                                   RPMOSTREED_TYPE_SYSROOT,
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);

  gdbus_interface_skeleton_class = G_DBUS_INTERFACE_SKELETON_CLASS (klass);
  gdbus_interface_skeleton_class->g_authorize_method = sysroot_authorize_method;
}

static gboolean
sysroot_reload_ostree_configs_and_deployments (RpmostreedSysroot *self,
                                               gboolean *out_changed,
                                               GError **error)
{
  gboolean ret = FALSE;
  gboolean did_change;

  /* reload ostree repo first so we pick up e.g. new remotes */
  if (!ostree_repo_reload_config (self->repo, NULL, error))
    goto out;
  if (!sysroot_populate_deployments_unlocked (self, &did_change, error))
    goto out;

  ret = TRUE;
  if (out_changed)
    *out_changed = did_change;
 out:
  if (ret && did_change)
    g_signal_emit (self, signals[UPDATED], 0);
  return ret;
}

gboolean
rpmostreed_sysroot_reload (RpmostreedSysroot *self, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Sysroot reload", error);
  return sysroot_reload_ostree_configs_and_deployments (self, NULL, error);
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
    sd_journal_print (LOG_ERR, "Unable to update state: %s", error->message);
}

static void
rpmostreed_sysroot_iface_init (RPMOSTreeSysrootIface *iface)
{
  iface->handle_create_osname = handle_create_osname;
  iface->handle_get_os = handle_get_os;
  iface->handle_register_client = handle_register_client;
  iface->handle_unregister_client = handle_unregister_client;
  iface->handle_reload = handle_reload;
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

  if (!reset_config_properties (self, error))
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

/* Ensures the sysroot is up to date, and returns references to the underlying
 * libostree sysroot object as well as the repo.  This function should
 * be used at the start of both state querying and transactions.
 */
gboolean
rpmostreed_sysroot_load_state (RpmostreedSysroot *self,
                               GCancellable *cancellable,
                               OstreeSysroot **out_sysroot,
                               OstreeRepo **out_repo,
                               GError **error)
{
  /* Always do a reload check here to suppress race conditions such as
   * doing: ostree admin pin && rpm-ostree cleanup
   * Without this we're relying on the file monitoring picking things up.
   * Note that the sysroot reload checks mtimes and hence is a cheap
   * no-op if nothing has changed.
   */
  if (!rpmostreed_sysroot_reload (self, error))
    return FALSE;
  if (out_sysroot)
    *out_sysroot = g_object_ref (rpmostreed_sysroot_get_root (self));
  if (out_repo)
    *out_repo = g_object_ref (rpmostreed_sysroot_get_repo (self));
  return TRUE;
}

gboolean
rpmostreed_sysroot_prep_for_txn (RpmostreedSysroot     *self,
                                 GDBusMethodInvocation *invocation,
                                 RpmostreedTransaction **out_compat_txn,
                                 GError               **error)
{
  if (self->transaction)
    {
      if (rpmostreed_transaction_is_compatible (self->transaction, invocation))
        {
          *out_compat_txn = g_object_ref (self->transaction);
          return TRUE;
        }
      const char *title = rpmostree_transaction_get_title ((RPMOSTreeTransaction*)(self->transaction));
      return glnx_throw (error, "Transaction in progress: %s", title);
    }
  *out_compat_txn = NULL;
  return TRUE;
}

gboolean
rpmostreed_sysroot_has_txn (RpmostreedSysroot     *self)
{
  return self->transaction != NULL;
}

static gboolean
on_force_close (gpointer data)
{
  RpmostreedSysroot *self = data;

  if (self->transaction)
    {
      rpmostreed_transaction_force_close (self->transaction);
      rpmostreed_sysroot_set_txn (self, NULL);
    }

  return FALSE;
}

static void
on_txn_executed_changed (GObject    *object,
                         GParamSpec *pspec,
                         gpointer    user_data)
{
  RpmostreedSysroot *self = user_data;
  gboolean executed;
  g_object_get (object, "executed", &executed, NULL);
  if (!executed && self->close_transaction_timeout_id == 0)
    {
      self->close_transaction_timeout_id =
        g_timeout_add_seconds (FORCE_CLOSE_TXN_TIMEOUT_SECS, on_force_close, self);
    }
}

void
rpmostreed_sysroot_set_txn (RpmostreedSysroot     *self,
                            RpmostreedTransaction *txn)
{
  /* If the transaction is changing, clear the timer */
  if (self->close_transaction_timeout_id > 0)
    {
      g_source_remove (self->close_transaction_timeout_id);
      self->close_transaction_timeout_id = 0;
    }

  if (txn != NULL)
    {
      g_assert (self->transaction == NULL);
      self->transaction = g_object_ref (txn);

      g_signal_connect (self->transaction, "notify::executed",
                        G_CALLBACK (on_txn_executed_changed), self);

      GDBusMethodInvocation *invocation = rpmostreed_transaction_get_invocation (self->transaction);
      g_autoptr(GVariant) v = g_variant_ref_sink (g_variant_new ("(sss)",
                                                                 g_dbus_method_invocation_get_method_name (invocation),
                                                                 g_dbus_method_invocation_get_object_path (invocation),
                                                                 g_dbus_method_invocation_get_sender (invocation)));
      rpmostree_sysroot_set_active_transaction ((RPMOSTreeSysroot*)self, v);
      rpmostree_sysroot_set_active_transaction_path ((RPMOSTreeSysroot*)self,
                                                     rpmostreed_transaction_get_client_address (self->transaction));
    }
  else
    {
      g_assert (self->transaction);
      g_clear_object (&self->transaction);

      g_autoptr(GVariant) v = g_variant_ref_sink (g_variant_new ("(sss)", "", "", ""));
      rpmostree_sysroot_set_active_transaction ((RPMOSTreeSysroot *)self, v);
      rpmostree_sysroot_set_active_transaction_path ((RPMOSTreeSysroot *)self, "");
    }
}

void
rpmostreed_sysroot_finish_txn (RpmostreedSysroot     *self,
                               RpmostreedTransaction *txn)
{
  g_assert (self->transaction == txn);
  rpmostreed_sysroot_set_txn (self, NULL);
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
