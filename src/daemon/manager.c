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

#include "daemon.h"
#include "manager.h"
#include "deployment.h"
#include "refspec.h"
#include "utils.h"
#include "auth.h"
#include "errors.h"

#include "libgsystem.h"

/**
 * SECTION:daemon
 * @title: Manager
 * @short_description: Implementation of #RPMOSTreeManager
 *
 * This type provides an implementation of the #RPMOSTreeManager interface.
 */

typedef struct _ManagerClass ManagerClass;

/**
 * Manager:
 *
 * The #Manager structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _Manager
{
  RPMOSTreeManagerSkeleton parent_instance;

  GCancellable *cancellable;

  gchar *sysroot_path;

  GMutex update_lock;

  GHashTable *deployments;
  GHashTable *refspecs;
  GRWLock children_lock;

  GFileMonitor *monitor;
  guint sig_changed;
  guint64 last_transaction_end;
};

struct _ManagerClass
{
  RPMOSTreeManagerSkeletonClass parent_class;
};

enum
{
  PROP_0,
  PROP_PATH,
};

enum {
  CANCEL_TASKS,
  UPDATED,
  NUM_SIGNALS
};

static guint64 TRANSACTION_THROTTLE_SECONDS = 2;
static guint manager_signals[NUM_SIGNALS] = { 0, };

static void manager_iface_init (RPMOSTreeManagerIface *iface);


G_DEFINE_TYPE_WITH_CODE (Manager, manager, RPMOSTREE_TYPE_MANAGER_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_MANAGER,
                                                manager_iface_init));

static Manager *_manager_instance;

/* ---------------------------------------------------------------------------------------------------- */


typedef struct {
  GVariant *result;
  GDBusMethodInvocation *invocation;
} DelayedInvocation;


static DelayedInvocation *
delayed_invocation_new (GDBusMethodInvocation *invocation,
                        GVariant *variant)
{
  DelayedInvocation *self = g_slice_new (DelayedInvocation);
  self->result = g_variant_ref (variant);
  self->invocation = g_object_ref (invocation);
  return self;
}


static void
delayed_invocation_free (DelayedInvocation *self)
{
  g_variant_unref (self->result);
  g_object_unref (self->invocation);
  g_slice_free (DelayedInvocation, self);
}


static void
delayed_invocation_invoke (Manager *manager, gpointer user_data)
{
  DelayedInvocation *self = user_data;
  g_dbus_method_invocation_return_value (self->invocation, self->result);
  g_signal_handlers_disconnect_by_data (manager, user_data);
}


/* ---------------------------------------------------------------------------------------------------- */

static void manager_ensure_refresh (Manager *self);

static void
manager_dispose (GObject *object)
{
  Manager *self = MANAGER (object);
  GHashTableIter diter;
  GHashTableIter riter;
  gpointer value;
  gint tries;

  g_cancellable_cancel (self->cancellable);

  if (self->monitor)
    {
      if (self->sig_changed)
        g_signal_handler_disconnect (self->monitor, self->sig_changed);
      self->sig_changed = 0;

      // HACK - It is not generally safe to just unref a GFileMonitor.
      // Some events might be on their way to the main loop from its
      // worker thread and if they arrive after the GFileMonitor has
      // been destroyed, bad things will happen.
      //
      // As a workaround, we cancel the monitor and then spin the main
      // loop a bit until nothing is pending anymore.
      //
      // https://bugzilla.gnome.org/show_bug.cgi?id=740491

      g_file_monitor_cancel (self->monitor);
      for (tries = 0; tries < 10; tries ++)
        {
          if (!g_main_context_iteration (NULL, FALSE))
            break;
        }
    }

  g_rw_lock_writer_lock (&self->children_lock);
  /* Tracked deployments are responsible to unpublish themselves */
  g_hash_table_iter_init (&diter, self->deployments);
  while (g_hash_table_iter_next (&diter, NULL, &value))
    g_object_run_dispose (value);
  g_hash_table_remove_all (self->deployments);

  /* Tracked refspecs are responsible to unpublish themselves */
  g_hash_table_iter_init (&riter, self->refspecs);
  while (g_hash_table_iter_next (&riter, NULL, &value))
    g_object_run_dispose (value);
  g_hash_table_remove_all (self->refspecs);
  g_rw_lock_writer_unlock (&self->children_lock);

  G_OBJECT_CLASS (manager_parent_class)->dispose (object);
}

static void
manager_finalize (GObject *object)
{
  Manager *self = MANAGER (object);
  _manager_instance = NULL;

  g_free (self->sysroot_path);

  g_hash_table_unref (self->deployments);
  g_hash_table_unref (self->refspecs);

  g_mutex_clear (&self->update_lock);
  g_rw_lock_clear (&self->children_lock);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->monitor);

  G_OBJECT_CLASS (manager_parent_class)->finalize (object);
}

static void
manager_init (Manager *self)
{
  g_assert (_manager_instance == NULL);
  _manager_instance = self;

  self->cancellable = g_cancellable_new ();

  self->deployments = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
  (GDestroyNotify) g_object_unref);
  self->refspecs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
  (GDestroyNotify) g_object_unref);

  g_mutex_init (&self->update_lock);
  g_rw_lock_init (&self->children_lock);

  self->monitor = NULL;
  self->sig_changed = 0;
  self->last_transaction_end = 0;
}

static void
manager_constructed (GObject *object)
{
  Manager *self = MANAGER (object);
  g_signal_connect (RPMOSTREE_MANAGER(self), "g-authorize-method",
                    G_CALLBACK (auth_check_root_or_access_denied), NULL);
  G_OBJECT_CLASS (manager_parent_class)->constructed (object);
}

static void
manager_set_property (GObject *object,
                      guint prop_id,
                      const GValue *value,
                      GParamSpec *pspec)
{
  Manager *self = MANAGER (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_assert (self->sysroot_path == NULL);
      self->sysroot_path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
manager_class_init (ManagerClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = manager_dispose;
  gobject_class->finalize     = manager_finalize;
  gobject_class->constructed  = manager_constructed;
  gobject_class->set_property = manager_set_property;

  /**
   * Manager:sysroot_path:
   *
   * The Sysroot path
   */
  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("sysroot-path",
                                                        NULL,
                                                        NULL,
                                                        NULL,
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  manager_signals[CANCEL_TASKS] = g_signal_new ("cancel-tasks",
                                                RPM_OSTREE_TYPE_DAEMON_MANAGER,
                                                G_SIGNAL_RUN_LAST,
                                                0, NULL, NULL,
                                                g_cclosure_marshal_generic,
                                                G_TYPE_NONE, 0);

  manager_signals[UPDATED] = g_signal_new ("interfaces-updated",
                                           RPM_OSTREE_TYPE_DAEMON_MANAGER,
                                           G_SIGNAL_RUN_LAST,
                                           0, NULL, NULL,
                                           g_cclosure_marshal_generic,
                                           G_TYPE_NONE, 0);
}


static GPtrArray *
_get_deployments_for_os (Manager *self,
                         const gchar *osname)
{
  GPtrArray *deployments = NULL;
  GHashTableIter iter;
  gpointer value;
  gboolean filter_by_os = FALSE;
  gint count = 0;

  filter_by_os = osname != NULL && strlen (osname) > 0;

  deployments = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  g_rw_lock_reader_lock (&self->children_lock);
  g_hash_table_iter_init (&iter, self->deployments);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      gs_free gchar *os = rpmostree_deployment_dup_osname (RPMOSTREE_DEPLOYMENT (value));
      if (!filter_by_os || g_strcmp0 (osname, os) == 0)
        {
          g_ptr_array_add (deployments, g_object_ref (value));
          count++;
        }
    }
  g_rw_lock_reader_unlock (&self->children_lock);

  g_ptr_array_sort (deployments, deployment_index_compare);

  return deployments;
}


static RefSpec *
_get_refspec_for_os (Manager *self,
                     const gchar *osname,
                     GError **error)
{
  RefSpec *refspec = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  Deployment *deployment = NULL;
  gs_free gchar *booted = NULL;
  gint i;

  deployments = _get_deployments_for_os (self, osname);
  booted = rpmostree_manager_dup_booted_deployment (RPMOSTREE_MANAGER (self));

  for (i=0; i<deployments->len; i++)
    {
      gs_free gchar *dbus_path = NULL;
      if (i == 0)
        deployment = deployments->pdata[0];

      g_object_get (deployments->pdata[i],
                    "dbus-path", &dbus_path, NULL);
      if (g_strcmp0(dbus_path, booted) == 0)
        {
          deployment = deployments->pdata[i];
          break;
        }
    }

  if (deployment)
    {
      refspec = deployment_get_refspec (deployment);
      if (!refspec)
        {
          g_set_error_literal (error,
                               RPM_OSTREED_ERROR,
                               RPM_OSTREED_ERROR_MISSING_REFSPEC,
                               "Could not find a valid deployment, You may need to rebase.");
        }
    }
  else
    {
      const gchar *error_message = NULL;
      if (osname != NULL)
        error_message = g_strdup_printf ("No previous deployment for OS '%s'", osname);
      else
        error_message = "No previous deployments found";

      g_set_error_literal (error,
                           RPM_OSTREED_ERROR,
                           RPM_OSTREED_ERROR_MISSING_DEPLOYMENT,
                           error_message);
    }

  return refspec;
}


static gboolean
_manager_add_refspec (Manager *self,
                      gchar *refspec_string,
                      OstreeRepo *ot_repo)
{
  RefSpec *refspec = NULL;
  gboolean ret = FALSE;

  refspec = g_hash_table_lookup (self->refspecs, refspec_string);
  if (refspec == NULL)
    {
      g_debug ("adding refspec %s", refspec_string);
      refspec = REFSPEC (refspec_new (refspec_string));
      if (refspec != NULL)
        {
          g_hash_table_insert (self->refspecs,
                               g_strdup (refspec_string),
                               g_object_ref (refspec));
          g_object_unref (refspec);
          ret = refspec_populate (refspec, refspec_string, ot_repo, TRUE);
        }
      else
        {
          g_warning ("Could not create refspec for %s", refspec_string);
        }
    }
  else
    {
      ret = refspec_populate (refspec, refspec_string, ot_repo, FALSE);
    }

  return ret;
}

static void
_manager_load_refspecs (Manager *self,
                        OstreeRepo *ot_repo)
{
  GHashTable *refs = NULL;
  GHashTableIter iter;
  GHashTableIter iter_old;
  GError *error = NULL;
  gpointer hashkey;
  gpointer value;

  // Add refspec interfaces
  if (!ostree_repo_list_refs (ot_repo,
                              NULL,
                              &refs,
                              self->cancellable,
                              &error))
    {
      g_warning ("Couldn't load refspecs %s", error->message);
      return;
    }

  g_rw_lock_writer_lock (&self->children_lock);

  // Remove no longer needed
  g_hash_table_iter_init (&iter_old, self->refspecs);
  while (g_hash_table_iter_next (&iter_old, &hashkey, &value))
    {
      if (!g_hash_table_contains (refs, hashkey) &&
          !refspec_is_updating (value))
        {
          // Dispose unpublishes
          g_object_run_dispose (G_OBJECT (value));
          g_hash_table_iter_remove (&iter_old);
        }
    }

  // Add all refs
  g_hash_table_iter_init (&iter, refs);
  while (g_hash_table_iter_next (&iter, &hashkey, NULL))
    _manager_add_refspec (self, hashkey, ot_repo);

  g_hash_table_remove_all (refs);
  g_hash_table_unref (refs);
  g_debug ("finished refspecs");

  g_rw_lock_writer_unlock (&self->children_lock);

  g_clear_error (&error);
}


static gboolean
_manager_add_deployment (Manager *self,
                         OstreeDeployment *ostree_deployment,
                         OstreeRepo *ot_repo,
                         gchar *id)
{
  Deployment *deployment = NULL;
  gboolean ret = FALSE;

  deployment = g_hash_table_lookup (self->deployments, id);
  if (deployment == NULL)
    {
      deployment = DEPLOYMENT (deployment_new (id));
      if (deployment != NULL)
        {
          g_hash_table_insert (self->deployments,
                               g_strdup (id),
                               g_object_ref (deployment));
          ret = deployment_populate (deployment, ostree_deployment, ot_repo, TRUE);
          g_object_unref (deployment);
        }
      else
        {
          g_warning ("Could not create deployment for %s", id);
        }
    }
  else
    {
      ret = deployment_populate (deployment, ostree_deployment, ot_repo, FALSE);
    }

  // if deployment has a refspec we don't know about, try to load it
  // but ignore errors.
  if (deployment)
    {
      gs_free gchar *ref_id = rpmostree_deployment_dup_origin_refspec (RPMOSTREE_DEPLOYMENT (deployment));
      if (!g_hash_table_contains (self->refspecs, ref_id))
        _manager_add_refspec (self, ref_id, ot_repo);
    }

  return ret;
}


static void
_manager_update_default_deployment (Manager *self,
                                    OstreeDeployment *ostree_deployment)
{
  gs_free gchar *id;
  gs_free gchar *path;

  id = deployment_generate_id (ostree_deployment);
  path = utils_generate_object_path (BASE_DBUS_PATH,
                                     DEPLOYMENT_DBUS_PATH_NAME,
                                     id, NULL);

  rpmostree_manager_set_default_deployment (RPMOSTREE_MANAGER (self),
                                            path ? path : "");
}


static void
_manager_update_booted_deployment (Manager *self,
                                   OstreeDeployment *ostree_deployment)
{
  gs_free gchar *id;
  gs_free gchar *path;

  id = deployment_generate_id (ostree_deployment);
  path = utils_generate_object_path (BASE_DBUS_PATH,
                                     DEPLOYMENT_DBUS_PATH_NAME,
                                     id, NULL);

  rpmostree_manager_set_booted_deployment (RPMOSTREE_MANAGER (self),
                                              path ? path : "");
}


static void
_manager_load_deployments (Manager *self,
                           OstreeSysroot *ot_sysroot,
                           OstreeRepo *ot_repo)
{
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  OstreeDeployment *booted = NULL;
  GHashTable *seen = NULL;

  GHashTableIter iter;
  gpointer hashkey;
  gpointer value;
  guint i;

  g_rw_lock_writer_lock (&self->children_lock);

  // Add deployment interfaces
  deployments = ostree_sysroot_get_deployments (ot_sysroot);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  // Returns NULL if sysroot is loading
  if (deployments != NULL)
    {
      for (i=0; i<deployments->len; i++)
        {
          gs_free gchar *id = deployment_generate_id (deployments->pdata[i]);
          _manager_add_deployment (self, deployments->pdata[i], ot_repo, id);
          g_hash_table_add (seen, g_strdup (id));
          if (i == 0)
            _manager_update_default_deployment (self, deployments->pdata[i]);
        }

      booted = ostree_sysroot_get_booted_deployment (ot_sysroot);
      if (booted)
        {
          _manager_update_booted_deployment (self, booted);
          g_object_unref (booted);
        }
    }

    // Remove dead deployments
    g_hash_table_iter_init (&iter, self->deployments);
    while (g_hash_table_iter_next (&iter, &hashkey, &value))
      {
        if (!g_hash_table_contains (seen, hashkey))
          {
            // Dispose unpublishes
            g_object_run_dispose (G_OBJECT (value));
            g_hash_table_iter_remove (&iter);
          }
      }
    g_hash_table_destroy (seen);

  g_rw_lock_writer_unlock (&self->children_lock);
  g_debug ("finished deployments");
}


static gboolean
manager_load_internals (Manager *self,
                        OstreeSysroot **out_sysroot,
                        OstreeRepo **out_repo,
                        GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object OstreeSysroot *ot_sysroot = NULL;
  gs_unref_object OstreeRepo *ot_repo = NULL;

  if (!utils_load_sysroot_and_repo (self->sysroot_path,
                                    self->cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    error))
      goto out;

  g_debug ("loading deployments and refspecs");
  _manager_load_refspecs (self, ot_repo);
  _manager_load_deployments (self, ot_sysroot, ot_repo);

  gs_transfer_out_value (out_sysroot, &ot_sysroot);
  if (out_repo != NULL)
    *out_repo = g_object_ref (ot_repo);

  ret = TRUE;

out:
  return ret;
}

static void
_do_reload_data (GTask         *task,
                 gpointer       object,
                 gpointer       data_ptr,
                 GCancellable  *cancellable)
{
  GError *error = NULL;
  Manager *self = MANAGER (object);
  gint i;

  g_debug ("reloading");
  for (i = 0; i < 3; i++)
    {
      if (manager_load_internals (self, NULL, NULL, &error))
        break;
      else
        g_message ("Error refreshing sysroot data: %s", error->message);
    }

  g_clear_error (&error);
}


static void
_reload_callback (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
    Manager *self = MANAGER (user_data);
    GTask *task = G_TASK (res);

    g_signal_emit (self, manager_signals[UPDATED], 0);
    g_object_unref (task);
}


static gboolean
_throttle_refresh (gpointer user_data)
{
  Manager *self = MANAGER (user_data);
  gboolean ret = TRUE;

  // Only run the update if there isn't another one
  // pending.
  g_rw_lock_writer_lock (&self->children_lock);
  if ((g_get_monotonic_time () - self->last_transaction_end) >
      TRANSACTION_THROTTLE_SECONDS * G_USEC_PER_SEC)
    {
      GTask *task = g_task_new (self, self->cancellable, _reload_callback, self);
      g_task_set_return_on_cancel (task, TRUE);
      self->last_transaction_end = 0;
      g_task_run_in_thread (task, _do_reload_data);
      ret = FALSE;
    }
  g_rw_lock_writer_unlock (&self->children_lock);

  return ret;
}

static void
on_repo_file (GFileMonitor *monitor,
                     GFile *file,
                     GFile *other_file,
                     GFileMonitorEvent event_type,
                     gpointer user_data)
{
  Manager *self = MANAGER (user_data);
  if (event_type == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
    {
      g_rw_lock_writer_lock (&self->children_lock);
      if (self->last_transaction_end == 0)
        g_timeout_add_seconds (TRANSACTION_THROTTLE_SECONDS,
                               _throttle_refresh,
                               user_data);

      self->last_transaction_end = g_get_monotonic_time ();
      g_rw_lock_writer_unlock (&self->children_lock);
    }
}


static gboolean
handle_get_ref_specs (RPMOSTreeManager *object,
                     GDBusMethodInvocation *invocation)
{

  Manager *self = MANAGER (object);

  GHashTableIter iter;
  gpointer hashkey;
  gpointer value;
  GVariantBuilder tbuilder;
  GVariantBuilder builder;
  GVariant *variant = NULL;

  g_variant_builder_init (&tbuilder, G_VARIANT_TYPE ("(a{so})"));
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{so}"));

  g_rw_lock_reader_lock (&self->children_lock);
  g_hash_table_iter_init (&iter, self->refspecs);
  while (g_hash_table_iter_next (&iter, &hashkey, &value))
    {
      gs_free gchar *path = NULL;
      g_object_get (REFSPEC (value), "dbus-path", &path, NULL);
      g_variant_builder_add (&builder, "{so}", hashkey, path);
    }
  g_rw_lock_reader_unlock (&self->children_lock);

  g_variant_builder_add_value (&tbuilder, g_variant_builder_end (&builder));
  variant = g_variant_builder_end (&tbuilder);

  // calling invoke directly to just pass the variant;
  g_dbus_method_invocation_return_value (invocation, variant);

  return TRUE;
}


static gboolean
handle_get_deployments (RPMOSTreeManager *object,
                        GDBusMethodInvocation *invocation,
                        GVariant *arg_options)
{
  Manager *self = MANAGER (object);

  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_free gchar *osname = NULL;
  GVariantBuilder tbuilder;
  GVariantBuilder builder;
  GVariant *variant = NULL;
  gint i;

  g_variant_builder_init (&tbuilder, G_VARIANT_TYPE ("(ao)"));
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("ao"));

  if (arg_options)
    g_variant_lookup (arg_options, "os", "s", &osname);
  deployments = _get_deployments_for_os (self, osname);

  for (i=0; i<deployments->len; i++)
    {
      gs_free gchar *path = NULL;
      g_object_get (deployments->pdata[i], "dbus-path", &path, NULL);
      g_variant_builder_add (&builder, "o", path);
    }

  g_variant_builder_add_value (&tbuilder, g_variant_builder_end (&builder));
  variant = g_variant_builder_end (&tbuilder);

  // calling invoke directly to just pass the variant;
  g_dbus_method_invocation_return_value (invocation, variant);

  return TRUE;
}


static gboolean
handle_cancel_operation (RPMOSTreeManager *object,
			 GDBusMethodInvocation *invocation)
{
  Manager *self = MANAGER (object);
  g_debug ("Canceling tasks");
  g_signal_emit (self, manager_signals[CANCEL_TASKS], 0);
  rpmostree_manager_complete_cancel_operation (object, invocation);
  return TRUE;
}


gboolean
manager_begin_update_operation (Manager *self,
                                GDBusMethodInvocation *invocation,
                                const gchar *type)
{
  gboolean ret;
  ret = g_mutex_trylock (&self->update_lock);
  if (!ret)
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   RPM_OSTREED_ERROR,
                                                   RPM_OSTREED_ERROR_UPDATE_IN_PROGRESS,
                                                   "Task already running");
  else
    rpmostree_manager_set_active_operation (RPMOSTREE_MANAGER (self),
					    type);
  return ret;
}


static void
emit_update_complete_after_refresh (Manager *self,
                                    gpointer user_data)
{
  gchar *message = user_data;
  rpmostree_manager_set_active_operation (RPMOSTREE_MANAGER (self),
                                        "idle");
  rpmostree_manager_emit_update_completed (RPMOSTREE_MANAGER (self),
                                          TRUE,
                                          message);
  g_signal_handlers_disconnect_by_data (self, user_data);

}


void
manager_end_update_operation (Manager *self,
                              gboolean success,
                              const gchar *message,
                              gboolean wait_for_refresh)
{
  if (!wait_for_refresh)
    {
      rpmostree_manager_set_active_operation (RPMOSTREE_MANAGER (self),
					      "idle");
      rpmostree_manager_emit_update_completed (RPMOSTREE_MANAGER (self),
                                              success,
                                              g_strdup (message));
    }
  else
    {
      g_debug ("waiting for update complete signal");
      g_signal_connect_data (self, "interfaces-updated",
                             G_CALLBACK (emit_update_complete_after_refresh),
                             g_strdup (message), (GClosureNotify) g_free, 0);
      manager_ensure_refresh (self);
    }

  g_mutex_unlock (&self->update_lock);
}


static gboolean
handle_get_upgrade_ref_spec (RPMOSTreeManager *object,
                             GDBusMethodInvocation *invocation,
                             GVariant *arg_options)
{
  gs_unref_object RefSpec *current_refspec = NULL;
  gs_free gchar *osname = NULL;

  GError *error = NULL; // freed take error call
  gchar *dpath = NULL; // freed by complete call

  Manager *self = MANAGER (object);

  if (arg_options)
    g_variant_lookup (arg_options, "os", "s", &osname);

  current_refspec = _get_refspec_for_os (self, osname, &error);
  if (!current_refspec)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  g_object_get (current_refspec, "dbus-path", &dpath, NULL);
  rpmostree_manager_complete_get_upgrade_ref_spec (object,
                                                   invocation,
                                                   dpath);

out:
  return TRUE;
}


static void
add_ref_spec_callback (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  gs_unref_object GTask *task = G_TASK (res);
  gchar *refspec = g_task_get_task_data (task); // freed with task

  gs_free gchar *dbus_path = NULL;
  GError *error = NULL; // freed by take error

  DelayedInvocation *di = NULL; // freed in callback
  GDBusMethodInvocation *invocation = user_data; // freed by di

  Manager *manager = MANAGER (source_object);


  if (!g_task_propagate_boolean (task, &error))
    goto out;

  dbus_path = utils_generate_object_path (BASE_DBUS_PATH,
                                          REFSPEC_DBUS_PATH_NAME,
                                          refspec, NULL);
  di = delayed_invocation_new (invocation,
                               g_variant_new ("(o)", dbus_path));
  g_signal_connect_data (manager, "interfaces-updated",
                         G_CALLBACK (delayed_invocation_invoke), di,
                         (GClosureNotify) delayed_invocation_free, 0);
  manager_ensure_refresh (manager);

out:
  if (error)
    g_dbus_method_invocation_take_error (invocation, error);
}


static gboolean
handle_add_ref_spec (RPMOSTreeManager *object,
                     GDBusMethodInvocation *invocation,
                     GVariant *arg_options,
                     const gchar *new_provided_refspec)
{

  Manager *self = MANAGER (object);
  GError *error = NULL;
  gs_free gchar *new_ref = NULL;
  gs_free gchar *new_remote = NULL;
  gs_free gchar *new_refspec = NULL;
  gs_free gchar *dpath = NULL;
  gs_unref_object RefSpec *current_refspec = NULL;
  gs_free gchar *osname = NULL;

  if (arg_options)
    g_variant_lookup (arg_options, "os", "s", &osname);

  current_refspec = _get_refspec_for_os (self, osname, NULL);

  if (!refspec_resolve_partial_aysnc (self,
                                      new_provided_refspec,
                                      current_refspec,
                                      add_ref_spec_callback,
                                      invocation,
                                      &error))
      g_dbus_method_invocation_take_error (invocation, error);

  return TRUE;
}

static void
manager_iface_init (RPMOSTreeManagerIface *iface)
{
  iface->handle_get_ref_specs = handle_get_ref_specs;
  iface->handle_get_upgrade_ref_spec = handle_get_upgrade_ref_spec;
  iface->handle_get_deployments = handle_get_deployments;
  iface->handle_add_ref_spec = handle_add_ref_spec;
  iface->handle_cancel_operation = handle_cancel_operation;
}





/**
 * manager_ensure_refresh:
 *
 * Ensures that the manager will reload it's
 * internal data.
 */

static void
manager_ensure_refresh (Manager *self)
{
  gboolean needs_run;
  g_rw_lock_reader_lock (&self->children_lock);
    needs_run = self->last_transaction_end == 0;
  g_rw_lock_reader_unlock (&self->children_lock);

  if (needs_run)
    _throttle_refresh (self);
}

/**
 * manager_create_and_validate :
 *
 * Creates a new Manager
 * sets up monitoring and publishes
 *
 * Returns: True on success
 */

gboolean
manager_populate (Manager *self,
                  GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object OstreeRepo *ot_repo = NULL;
  gs_unref_object GFile *transaction_file = NULL;

  g_return_val_if_fail (self != NULL, FALSE);

  if (!manager_load_internals (self, NULL, &ot_repo, error))
    goto out;

  if (self->monitor == NULL)
    {
      GFile *repo_file = ostree_repo_get_path (ot_repo); // owned by ot_repo
      self->monitor = g_file_monitor (repo_file, 0, NULL, error);

      if (self->monitor == NULL)
        goto out;

      self->sig_changed = g_signal_connect (self->monitor,
                                            "changed",
                                            G_CALLBACK (on_repo_file),
                                            self);
    }

  ret = TRUE;

out:
  return ret;
}

/**
 * manager_get_sysroot_path :
 *
 * Returns: The filesystem path for sysroot.
 * Do not free owned by manager
 */
gchar *
manager_get_sysroot_path (Manager *self)
{
  return self->sysroot_path;
}


/**
 * manager_get:
 *
 * Returns: (transfer none): The singleton #Manager instance
 */
Manager *
manager_get (void)
{
  g_assert (_manager_instance);
  return _manager_instance;
}



/* ---------------------------------------------------------------------------------------------------- */
