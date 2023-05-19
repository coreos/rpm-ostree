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

#include "rpmostree-cxxrs.h"
#include "rpmostree-util.h"
#include "rpmostreed-daemon.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-errors.h"
#include "rpmostreed-os-experimental.h"
#include "rpmostreed-os.h"
#include "rpmostreed-sysroot.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-utils.h"

#include "rpmostree-output.h"

#include "libglnx.h"
#include <err.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <systemd/sd-journal.h>
#include <systemd/sd-login.h>

/* Avoid clients leaking their bus connections keeping the transaction open */
#define FORCE_CLOSE_TXN_TIMEOUT_SECS 30

static gboolean sysroot_reload_ostree_configs_and_deployments (RpmostreedSysroot *self,
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
struct _RpmostreedSysroot
{
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

struct _RpmostreedSysrootClass
{
  RPMOSTreeSysrootSkeletonClass parent_class;
};

enum
{
  UPDATED,
  NUM_SIGNALS
};

static guint signals[NUM_SIGNALS];
static void rpmostreed_sysroot_iface_init (RPMOSTreeSysrootIface *iface);

G_DEFINE_TYPE_WITH_CODE (RpmostreedSysroot, rpmostreed_sysroot, RPMOSTREE_TYPE_SYSROOT_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_SYSROOT,
                                                rpmostreed_sysroot_iface_init));

static RpmostreedSysroot *_sysroot_instance;

/* ----------------------------------------------------------------------------------------------------
 */

static gboolean
handle_get_os (RPMOSTreeSysroot *object, GDBusMethodInvocation *invocation, const char *arg_name)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);

  if (arg_name[0] == '\0')
    {
      rpmostree_sysroot_complete_get_os (object, invocation, rpmostree_sysroot_dup_booted (object));
      return FALSE;
    }

  g_autoptr (GDBusInterfaceSkeleton) os_interface
      = (GDBusInterfaceSkeleton *)g_hash_table_lookup (self->os_interfaces, arg_name);
  if (os_interface != NULL)
    g_object_ref (os_interface);

  if (os_interface != NULL)
    {
      const char *object_path = g_dbus_interface_skeleton_get_object_path (os_interface);
      rpmostree_sysroot_complete_get_os (object, invocation, object_path);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                             "OS name \"%s\" not found", arg_name);
    }

  return TRUE;
}

static gboolean
sysroot_populate_deployments_unlocked (RpmostreedSysroot *self, gboolean *out_changed,
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

  const gboolean repo_changed
      = !((self->repo_last_stat.st_mtim.tv_sec == repo_new_stat.st_mtim.tv_sec)
          && (self->repo_last_stat.st_mtim.tv_nsec == repo_new_stat.st_mtim.tv_nsec));
  if (repo_changed)
    self->repo_last_stat = repo_new_stat;

  if (!(sysroot_changed || repo_changed))
    return TRUE; /* Note early return */

  g_debug ("loading deployments");

  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));

  g_autoptr (GHashTable) seen_osnames = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);

  /* Updated booted property; object owned by sysroot */
  g_autofree gchar *booted_id = NULL;
  OstreeDeployment *booted = ostree_sysroot_get_booted_deployment (self->ot_sysroot);
  if (booted)
    {
      auto os = ostree_deployment_get_osname (booted);

      CXX_TRY_VAR (path,
                   rpmostreecxx::generate_object_path (rust::Str (BASE_DBUS_PATH), rust::Str (os)),
                   error);

      rpmostree_sysroot_set_booted (RPMOSTREE_SYSROOT (self), path.c_str ());
      auto bootedid_v = rpmostreecxx::deployment_generate_id (*booted);
      booted_id = g_strdup (bootedid_v.c_str ());
    }
  else
    {
      rpmostree_sysroot_set_booted (RPMOSTREE_SYSROOT (self), "/");
    }

  /* Add deployment interfaces */
  g_autoptr (GPtrArray) deployments = ostree_sysroot_get_deployments (self->ot_sysroot);

  for (guint i = 0; deployments != NULL && i < deployments->len; i++)
    {
      auto deployment = static_cast<OstreeDeployment *> (deployments->pdata[i]);
      GVariant *variant = NULL;
      if (!rpmostreed_deployment_generate_variant (self->ot_sysroot, deployment, booted_id,
                                                   self->repo, TRUE, &variant, error))
        return glnx_prefix_error (error, "Reading deployment %u", i);

      g_variant_builder_add_value (&builder, variant);

      const char *deployment_os = ostree_deployment_get_osname (deployment);

      /* Have we not seen this osname instance before?  If so, add it
       * now.
       */
      if (!g_hash_table_contains (self->os_interfaces, deployment_os))
        {
          RPMOSTreeOS *obj = rpmostreed_os_new (self->ot_sysroot, self->repo, deployment_os, error);
          if (!obj)
            return FALSE;

          g_hash_table_insert (self->os_interfaces, g_strdup (deployment_os), obj);

          RPMOSTreeOSExperimental *eobj
              = rpmostreed_osexperimental_new (self->ot_sysroot, self->repo, deployment_os, error);
          if (!eobj)
            return FALSE;
          g_hash_table_insert (self->osexperimental_interfaces, g_strdup (deployment_os), eobj);
        }
      /* Owned by deployment, hash lifetime is smaller */
      g_hash_table_add (seen_osnames, (char *)deployment_os);
    }

  /* Remove dead os paths */
  GLNX_HASH_TABLE_FOREACH_IT (self->os_interfaces, it, const char *, k, GObject *, v)
    {
      if (!g_hash_table_contains (seen_osnames, k))
        {
          g_object_run_dispose (G_OBJECT (v));
          g_hash_table_iter_remove (&it);

          g_hash_table_remove (self->osexperimental_interfaces, k);
        }
    }

  rpmostree_sysroot_set_deployments (RPMOSTREE_SYSROOT (self), g_variant_builder_end (&builder));
  g_debug ("finished deployments");

  if (out_changed)
    *out_changed = TRUE;
  return TRUE;
}

static gboolean
handle_register_client (RPMOSTreeSysroot *object, GDBusMethodInvocation *invocation,
                        GVariant *arg_options)
{
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_assert (sender);

  g_autoptr (GVariantDict) optdict = g_variant_dict_new (arg_options);
  const char *client_id = NULL;
  g_variant_dict_lookup (optdict, "id", "&s", &client_id);

  rpmostreed_daemon_add_client (rpmostreed_daemon_get (), sender, client_id);
  rpmostree_sysroot_complete_register_client (object, invocation);

  return TRUE;
}

static gboolean
handle_unregister_client (RPMOSTreeSysroot *object, GDBusMethodInvocation *invocation,
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
reset_config_properties (RpmostreedSysroot *self, GError **error)
{
  RpmostreedDaemon *daemon = rpmostreed_daemon_get ();

  RpmostreedAutomaticUpdatePolicy policy = rpmostreed_get_automatic_update_policy (daemon);
  const char *policy_str = rpmostree_auto_update_policy_to_str (policy, NULL);
  g_assert (policy_str);
  rpmostree_sysroot_set_automatic_update_policy (RPMOSTREE_SYSROOT (self), policy_str);

  return TRUE;
}

typedef struct
{
  RPMOSTreeSysroot *object;
  GDBusMethodInvocation *invocation;
} ReloadIdleData;

/* See comment below for why we defer to an idle for this */
static gboolean
handle_reload_via_idle (gpointer data)
{
  auto idata = static_cast<ReloadIdleData *> (data);
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (idata->object);
  GDBusMethodInvocation *invocation = idata->invocation;

  g_autoptr (GError) local_error = NULL;
  if (!sysroot_populate_deployments_unlocked (self, NULL, &local_error))
    {
      g_prefix_error (&local_error, "Handling reload: ");
      g_dbus_method_invocation_take_error (invocation, util::move_nullify (local_error));
      return G_SOURCE_REMOVE;
    }

  /* always send an UPDATED signal to also force OS interfaces to reload */
  g_signal_emit (self, signals[UPDATED], 0);

  rpmostree_sysroot_complete_reload (idata->object, invocation);

  return G_SOURCE_REMOVE;
}

/* reloads *only* deployments and os internals, *no* configuration files */
static gboolean
handle_reload (RPMOSTreeSysroot *object, GDBusMethodInvocation *invocation)
{
  ReloadIdleData *idata = g_new0 (ReloadIdleData, 1);
  idata->object = object;
  idata->invocation = invocation;
  /* Deferred to an idle to ensure that we've processed any
   * other pending notifications, such as file descriptors being closed
   * for the transaction, etc.
   */
  g_idle_add_full (G_PRIORITY_LOW, handle_reload_via_idle, idata, g_free);
  return TRUE;
}

static gboolean
reload_config (RpmostreedSysroot *self, GError **error)
{
  g_assert (self != NULL);

  gboolean config_changed = FALSE;
  if (!rpmostreed_daemon_reload_config (rpmostreed_daemon_get (), &config_changed, error))
    return glnx_prefix_error (error, "Reloading daemon configuration");

  if (config_changed && !reset_config_properties (self, error))
    return glnx_prefix_error (error, "Remapping properties");

  gboolean sysroot_changed = FALSE;
  if (!sysroot_reload_ostree_configs_and_deployments (self, &sysroot_changed, error))
    return glnx_prefix_error (error, "Reloading ostree details");

  /* also send an UPDATED signal if configs changed to cause OS interfaces to reload; we do
   * it here if not done already in `rpmostreed_sysroot_reload` */
  if (config_changed && !sysroot_changed)
    g_signal_emit (self, signals[UPDATED], 0);

  return TRUE;
}

/* reloads *everything*: ostree configs, rpm-ostreed.conf, deployments, os internals */
static gboolean
handle_reload_config (RPMOSTreeSysroot *object, GDBusMethodInvocation *invocation)
{
  g_autoptr (GError) local_error = NULL;
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (object);

  gboolean is_ok = reload_config (self, &local_error);
  if (!is_ok)
    {
      g_assert (local_error != NULL);
      g_prefix_error (&local_error, "Handling config reload: ");
      g_dbus_method_invocation_take_error (invocation, util::move_nullify (local_error));
      return TRUE; /* 🔚 Early return */
    }

  rpmostree_sysroot_complete_reload_config (object, invocation);

  return TRUE;
}

/* ----------------------------------------------------------------------------------------------------
 */
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
      for (tries = 0; tries < 10; tries++)
        {
          if (!g_main_context_iteration (NULL, FALSE))
            break;
        }
    }

  /* Tracked os paths are responsible to unpublish themselves */
  GLNX_HASH_TABLE_FOREACH_KV (self->os_interfaces, const char *, k, GObject *, value)
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

  G_OBJECT_CLASS (rpmostreed_sysroot_parent_class)->finalize (object);
}

static void
rpmostreed_sysroot_init (RpmostreedSysroot *self)
{
  g_assert (_sysroot_instance == NULL);
  _sysroot_instance = self;

  self->os_interfaces
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_object_unref);
  self->osexperimental_interfaces
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_object_unref);

  self->monitor = NULL;
}

static gboolean
sysroot_authorize_method (GDBusInterfaceSkeleton *interface, GDBusMethodInvocation *invocation)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (interface);
  const gchar *method_name = g_dbus_method_invocation_get_method_name (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  gboolean authorized = FALSE;
  g_autoptr (GError) local_error = NULL;
  const char *action = NULL;

  if (!rpmostreed_sysroot_authorize_direct (self, invocation, &authorized, &local_error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Failed to load polkit: %s", local_error->message);
      return FALSE;
    }
  if (authorized)
    return TRUE;

  // For historical reasons, we default to allowing interactive auth.
  // See https://github.com/coreos/rpm-ostree/issues/4415
  // TODO: In the future we need to honor this.
  // GDBusMessage *invoker_message = g_dbus_method_invocation_get_message (invocation);
  // GDBusMessageFlags invoker_flags = g_dbus_message_get_flags (invoker_message);
  // bool invoker_allows_interactive_auth = (invoker_flags &
  // G_DBUS_MESSAGE_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION) > 0;
  bool allow_interactive_auth = true;

  if (g_strcmp0 (method_name, "GetOS") == 0 || g_strcmp0 (method_name, "Reload") == 0)
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
  else if (g_strcmp0 (method_name, "RegisterClient") == 0
           || g_strcmp0 (method_name, "UnregisterClient") == 0)
    {
      // We never do interactive auth for these
      allow_interactive_auth = false;
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
                  sd_journal_print (LOG_INFO, "Allowing active client %s (uid %d)", sender, uid);
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
      g_autoptr (GError) local_error = NULL;

      g_autoptr (PolkitAuthority) authority
          = rpmostreed_sysroot_get_polkit_authority (self, &local_error);
      if (!authority)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Failed to load polkit: %s", local_error->message);
          return FALSE;
        }

      int authflags = 0;
      if (allow_interactive_auth)
        authflags |= POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION;
      glnx_unref_object PolkitAuthorizationResult *result
          = polkit_authority_check_authorization_sync (
              self->authority, subject, action, NULL,
              static_cast<PolkitCheckAuthorizationFlags> (authflags), NULL, &local_error);
      if (result == NULL)
        {
          g_assert (local_error);
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Authorization error: %s", local_error->message);
          return FALSE;
        }

      authorized = polkit_authorization_result_get_is_authorized (result);
    }

  if (!authorized)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
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
  gobject_class->finalize = sysroot_finalize;

  signals[UPDATED] = g_signal_new ("updated", RPMOSTREED_TYPE_SYSROOT, G_SIGNAL_RUN_LAST, 0, NULL,
                                   NULL, NULL, G_TYPE_NONE, 0);

  gdbus_interface_skeleton_class = G_DBUS_INTERFACE_SKELETON_CLASS (klass);
  gdbus_interface_skeleton_class->g_authorize_method = sysroot_authorize_method;
}

static gboolean
sysroot_reload_ostree_configs_and_deployments (RpmostreedSysroot *self, gboolean *out_changed,
                                               GError **error)
{
  gboolean did_change = FALSE;

  /* reload ostree repo first so we pick up e.g. new remotes */
  if (!ostree_repo_reload_config (self->repo, NULL, error))
    return FALSE;
  if (!sysroot_populate_deployments_unlocked (self, &did_change, error))
    return FALSE;

  if (out_changed != NULL)
    *out_changed = did_change;

  if (did_change)
    g_signal_emit (self, signals[UPDATED], 0);

  return TRUE;
}

gboolean
rpmostreed_sysroot_reload (RpmostreedSysroot *self, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Sysroot reload", error);
  return sysroot_reload_ostree_configs_and_deployments (self, NULL, error);
}

static void
on_deploy_changed (GFileMonitor *monitor, GFile *file, GFile *other_file,
                   GFileMonitorEvent event_type, gpointer user_data)
{
  RpmostreedSysroot *self = RPMOSTREED_SYSROOT (user_data);
  g_autoptr (GError) error = NULL;

  if (event_type == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
    {
      if (!rpmostreed_sysroot_reload (self, &error))
        sd_journal_print (LOG_ERR, "Unable to update state: %s", error->message);
    }
}

static void
rpmostreed_sysroot_iface_init (RPMOSTreeSysrootIface *iface)
{
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
rpmostreed_sysroot_populate (RpmostreedSysroot *self, GCancellable *cancellable, GError **error)
{
  g_assert (self != NULL);

  /* See also related code in rpmostred-transaction.cxx */
  const char *sysroot_path = rpmostree_sysroot_get_path (RPMOSTREE_SYSROOT (self));
  g_autoptr (GFile) sysroot_file = g_file_new_for_path (sysroot_path);
  self->ot_sysroot = ostree_sysroot_new (sysroot_file);
  if (!ostree_sysroot_initialize (self->ot_sysroot, error))
    return FALSE;
  ostree_sysroot_set_mount_namespace_in_use (self->ot_sysroot);

  /* This creates and caches an OstreeRepo instance inside
   * OstreeSysroot to ensure subsequent ostree_sysroot_get_repo()
   * calls won't fail.
   */
  if (!ostree_sysroot_get_repo (self->ot_sysroot, &self->repo, cancellable, error))
    return FALSE;

  if (!sysroot_populate_deployments_unlocked (self, NULL, error))
    return FALSE;

  ROSCXX_TRY (daemon_sanitycheck_environment (*self->ot_sysroot), error);

  if (!reset_config_properties (self, error))
    return FALSE;

  if (self->monitor == NULL)
    {
      const char *sysroot_path
          = gs_file_get_path_cached (ostree_sysroot_get_path (self->ot_sysroot));
      g_autofree char *sysroot_deploy_path = g_build_filename (sysroot_path, "ostree/deploy", NULL);
      g_autoptr (GFile) sysroot_deploy = g_file_new_for_path (sysroot_deploy_path);

      self->monitor = g_file_monitor (sysroot_deploy, (GFileMonitorFlags)0, NULL, error);

      if (self->monitor == NULL)
        return FALSE;

      self->sig_changed
          = g_signal_connect (self->monitor, "changed", G_CALLBACK (on_deploy_changed), self);
    }

  return TRUE;
}

/* Ensures the sysroot is up to date, and returns references to the underlying
 * libostree sysroot object as well as the repo.  This function should
 * be used at the start of both state querying and transactions.
 */
gboolean
rpmostreed_sysroot_load_state (RpmostreedSysroot *self, GCancellable *cancellable,
                               OstreeSysroot **out_sysroot, OstreeRepo **out_repo, GError **error)
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
    *out_sysroot = (OstreeSysroot *)g_object_ref (rpmostreed_sysroot_get_root (self));
  if (out_repo)
    *out_repo = (OstreeRepo *)g_object_ref (rpmostreed_sysroot_get_repo (self));
  return TRUE;
}

gboolean
rpmostreed_sysroot_prep_for_txn (RpmostreedSysroot *self, GDBusMethodInvocation *invocation,
                                 RpmostreedTransaction **out_compat_txn, GError **error)
{
  if (rpmostreed_daemon_is_rebooting (rpmostreed_daemon_get ()))
    return glnx_throw (error, "Reboot initiated, cannot start new transaction");
  if (self->transaction)
    {
      if (rpmostreed_transaction_is_compatible (self->transaction, invocation))
        {
          sd_journal_print (LOG_INFO, "Client joining existing transaction");
          *out_compat_txn = (RpmostreedTransaction *)g_object_ref (self->transaction);
          return TRUE;
        }
      const char *title
          = rpmostree_transaction_get_title ((RPMOSTreeTransaction *)(self->transaction));
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                   "Transaction in progress: %s\n You can cancel the current transaction "
                   "with `rpm-ostree cancel`",
                   title);
      return FALSE;
    }
  *out_compat_txn = NULL;
  return TRUE;
}

gboolean
rpmostreed_sysroot_has_txn (RpmostreedSysroot *self)
{
  return self->transaction != NULL;
}

static gboolean
on_force_close (gpointer data)
{
  auto self = static_cast<RpmostreedSysroot *> (data);

  if (self->transaction)
    {
      sd_journal_print (LOG_WARNING, "Forcibly closing transaction due to timeout");
      rpmostreed_transaction_force_close (self->transaction);
      rpmostreed_sysroot_set_txn (self, NULL);
    }

  return FALSE;
}

static void
on_txn_executed_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  auto self = static_cast<RpmostreedSysroot *> (user_data);
  gboolean executed;
  g_object_get (object, "executed", &executed, NULL);
  if (executed && self->close_transaction_timeout_id == 0)
    {
      self->close_transaction_timeout_id
          = g_timeout_add_seconds (FORCE_CLOSE_TXN_TIMEOUT_SECS, on_force_close, self);
    }
}

void
rpmostreed_sysroot_set_txn (RpmostreedSysroot *self, RpmostreedTransaction *txn)
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
      self->transaction = (RpmostreedTransaction *)g_object_ref (txn);

      g_signal_connect (self->transaction, "notify::executed", G_CALLBACK (on_txn_executed_changed),
                        self);

      GDBusMethodInvocation *invocation = rpmostreed_transaction_get_invocation (self->transaction);
      g_autoptr (GVariant) v = g_variant_ref_sink (
          g_variant_new ("(sss)", g_dbus_method_invocation_get_method_name (invocation),
                         g_dbus_method_invocation_get_sender (invocation),
                         g_dbus_method_invocation_get_object_path (invocation)));
      rpmostree_sysroot_set_active_transaction ((RPMOSTreeSysroot *)self, v);
      rpmostree_sysroot_set_active_transaction_path (
          (RPMOSTreeSysroot *)self, rpmostreed_transaction_get_client_address (self->transaction));
    }
  else
    {
      g_assert (self->transaction);
      g_clear_object (&self->transaction);

      g_autoptr (GVariant) v = g_variant_ref_sink (g_variant_new ("(sss)", "", "", ""));
      rpmostree_sysroot_set_active_transaction ((RPMOSTreeSysroot *)self, v);
      rpmostree_sysroot_set_active_transaction_path ((RPMOSTreeSysroot *)self, "");
    }
}

void
rpmostreed_sysroot_set_txn_and_title (RpmostreedSysroot *self, RpmostreedTransaction *txn,
                                      const char *title)
{
  rpmostree_transaction_set_title ((RPMOSTreeTransaction *)txn, title);
  rpmostreed_sysroot_set_txn (self, txn);
}

void
rpmostreed_sysroot_finish_txn (RpmostreedSysroot *self, RpmostreedTransaction *txn)
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

// Default method that always authorizes a caller with uid 0 for anything.
// systemd upstream today goes to a next level of getting the remote pid,
// then from there gathering the capabilities
// from /proc to check for CAP_SYS_ADMIN in most cases.  That's nice but also currently
// racy if the process exits or forks and passes the dbus connection around for example.
// We'll just be OK with uid == 0.  SELinux should put a stop to processes
// that run as uid 0 but shouldn't talk to us from working.
static gboolean
authorize_method_for_uid0 (GDBusMethodInvocation *invocation, gboolean *out_authorized,
                           GError **error)
{
  *out_authorized = FALSE;

  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr (GVariant) res = g_dbus_connection_call_sync (
      g_dbus_method_invocation_get_connection (invocation), "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixUser",
      g_variant_new ("(s)", sender), (GVariantType *)"(u)", G_DBUS_CALL_FLAGS_NONE, -1, NULL,
      error);
  if (!res)
    return FALSE;
  guint32 uid = 1;
  g_variant_get (res, "(u)", &uid);
  *out_authorized = (uid == 0);
  return TRUE;
}

// Check whether a method call is authorized without consulting PolicyKit.
gboolean
rpmostreed_sysroot_authorize_direct (RpmostreedSysroot *self, GDBusMethodInvocation *invocation,
                                     gboolean *out_is_authorized, GError **error)
{
  *out_is_authorized = FALSE;
  if (self->on_session_bus)
    {
      /* The daemon is on the session bus, running self tests */
      *out_is_authorized = TRUE;
      return TRUE;
    }
  // Now, we check via direct credentials
  // https://github.com/coreos/rpm-ostree/issues/3554
  return authorize_method_for_uid0 (invocation, out_is_authorized, error);
}

// Acquire a reference to the polkit authority; this may successfully return
// NULL for *out_authority if polkit is not needed.
PolkitAuthority *
rpmostreed_sysroot_get_polkit_authority (RpmostreedSysroot *self, GError **error)
{
  if (!self->authority)
    {
      /* Note that confusingly, this method will *not* error out if polkit isn't installed.
       * This is because glib has logic to assume that e.g. "NameHasNoOwner" is non-fatal,
       * and to instead wait for the name to appear, only erroring when one tries to invoke
       * a method.
       *
       * https://gitlab.gnome.org/GNOME/glib/-/blob/bd63436fadf5f36001fa5223d3f4e6dc0a7d56cc/gio/gdbusproxy.c#L1534
       * */
      self->authority = polkit_authority_get_sync (NULL, error);
      if (!self->authority)
        return NULL;
    }
  return (PolkitAuthority *)g_object_ref (self->authority);
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
