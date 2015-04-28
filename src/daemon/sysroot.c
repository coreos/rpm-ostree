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

#include "sysroot.h"
#include "daemon.h"
#include "deployment.h"
#include "refspec.h"
#include "utils.h"
#include "auth.h"
#include "errors.h"

#include "libgsystem.h"

/**
 * SECTION:daemon
 * @title: Sysroot
 * @short_description: Implementation of #RPMOSTreeSysroot
 *
 * This type provides an implementation of the #RPMOSTreeSysroot interface.
 */

typedef struct _SysrootClass SysrootClass;

/**
 * Sysroot:
 *
 * The #Sysroot structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _Sysroot
{
  RPMOSTreeSysrootSkeleton parent_instance;

  GCancellable *cancellable;

  gchar *path;
  gchar *dbus_name;
  gchar *dbus_path;

  GMutex update_lock;

  GHashTable *deployments;
  GHashTable *refspecs;
  GRWLock children_lock;

  GFileMonitor *monitor;
  guint sig_changed;
  guint64 last_transaction_end;
};

struct _SysrootClass
{
  RPMOSTreeSysrootSkeletonClass parent_class;
};

enum
{
  PROP_0,
  PROP_PATH,
  PROP_DBUS_NAME,
  PROP_DBUS_PATH,
};

enum {
  CANCEL_TASKS,
  UPDATED,
  NUM_SIGNALS
};

static guint64 TRANSACTION_THROTTLE_SECONDS = 2;
static guint sysroot_signals[NUM_SIGNALS] = { 0, };

static void sysroot_iface_init (RPMOSTreeSysrootIface *iface);

G_DEFINE_TYPE_WITH_CODE (Sysroot, sysroot, RPMOSTREE_TYPE_SYSROOT_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_SYSROOT,
                                                sysroot_iface_init));

static void sysroot_ensure_refresh (Sysroot *self);
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
delayed_invocation_invoke (Sysroot *sysroot, gpointer user_data)
{
  DelayedInvocation *self = user_data;
  g_dbus_method_invocation_return_value (self->invocation, self->result);
  g_signal_handlers_disconnect_by_data (sysroot, user_data);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
sysroot_dispose (GObject *object)
{
  Sysroot *self = SYSROOT (object);
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

  daemon_unpublish (daemon_get (), self->dbus_path, self);

  G_OBJECT_CLASS (sysroot_parent_class)->dispose (object);
}

static void
sysroot_finalize (GObject *object)
{
  Sysroot *self = SYSROOT (object);

  g_free (self->path);
  g_free (self->dbus_path);
  g_free (self->dbus_name);

  g_hash_table_unref (self->deployments);
  g_hash_table_unref (self->refspecs);

  g_mutex_clear (&self->update_lock);
  g_rw_lock_clear (&self->children_lock);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->monitor);

  G_OBJECT_CLASS (sysroot_parent_class)->finalize (object);
}


static void
sysroot_init (Sysroot *sysroot)
{
  sysroot->cancellable = g_cancellable_new ();

  sysroot->deployments = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
  (GDestroyNotify) g_object_unref);
  sysroot->refspecs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
  (GDestroyNotify) g_object_unref);

  g_mutex_init (&sysroot->update_lock);
  g_rw_lock_init (&sysroot->children_lock);

  sysroot->monitor = NULL;
  sysroot->sig_changed = 0;
  sysroot->last_transaction_end = 0;
}


gchar *
sysroot_generate_sub_object_path (Sysroot *self,
                                  const gchar *part,
                                  ...)
{
  gchar *result;
  va_list va;

  va_start (va, part);
  result = utils_generate_object_path_from_va (self->dbus_path, part, va);
  va_end (va);

  return result;
}


static GPtrArray *
_get_deployments_for_os (Sysroot *self,
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
_get_refspec_for_os (Sysroot *self,
                     const gchar *osname,
                     GError **error)
{
  RefSpec *refspec = NULL;
  GPtrArray *deployments = NULL;
  Deployment *deployment = NULL;
  gs_free gchar *booted = NULL;
  gint i;

  deployments = _get_deployments_for_os (self, osname);
  booted = rpmostree_sysroot_dup_booted_deployment (RPMOSTREE_SYSROOT (self));

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
  g_ptr_array_unref (deployments);

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
  g_ptr_array_unref (deployments);

  return refspec;
}


static gboolean
_sysroot_add_deployment (Sysroot *self,
                        OstreeDeployment *ostree_deployment,
                        OstreeRepo *ot_repo,
                        gchar *id)
{
  Deployment *deployment = NULL;
  gboolean ret = FALSE;

  deployment = g_hash_table_lookup (self->deployments, id);
  if (deployment == NULL)
    {
      deployment = DEPLOYMENT (deployment_new (self, id));
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
        refspec_resolve_partial_aysnc (self, ref_id, NULL, NULL, NULL, NULL);
    }

  return ret;
}


static void
_sysroot_update_default_deployment (Sysroot *self,
                                    OstreeDeployment *ostree_deployment)
{
  gs_free gchar *id;
  gs_free gchar *path;

  id = deployment_generate_id (ostree_deployment);
  path = sysroot_generate_sub_object_path (self, DEPLOYMENT_DBUS_PATH_NAME, id, NULL);

  rpmostree_sysroot_set_default_deployment (RPMOSTREE_SYSROOT (self),
                                            path ? path : "");
}


static void
_sysroot_update_booted_deployment (Sysroot *self,
                                   OstreeDeployment *ostree_deployment)
{
  gs_free gchar *id;
  gs_free gchar *path;

  id = deployment_generate_id (ostree_deployment);
  path = sysroot_generate_sub_object_path (self, DEPLOYMENT_DBUS_PATH_NAME, id, NULL);

  rpmostree_sysroot_set_booted_deployment (RPMOSTREE_SYSROOT (self),
                                              path ? path : "");
}


static void
_sysroot_load_deployments (Sysroot *self,
                           OstreeSysroot *ot_sysroot,
                           OstreeRepo *ot_repo)
{
  GPtrArray *deployments = NULL;
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
          gchar *id = deployment_generate_id (deployments->pdata[i]);
          _sysroot_add_deployment (self, deployments->pdata[i], ot_repo, id);
          g_hash_table_add (seen, g_strdup (id));
          if (i == 0)
            _sysroot_update_default_deployment (self, deployments->pdata[i]);
          g_free (id);
        }
      g_ptr_array_unref (deployments);

      booted = ostree_sysroot_get_booted_deployment (ot_sysroot);
      if (booted)
        {
          _sysroot_update_booted_deployment (self, booted);
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
_sysroot_add_refspec (Sysroot *self,
                      const gchar *refspec_string,
                      OstreeRepo *ot_repo)
{
  RefSpec *refspec = NULL;
  gboolean ret = FALSE;

  refspec = g_hash_table_lookup (self->refspecs, refspec_string);
  if (refspec == NULL)
    {
      g_debug ("adding refspec %s", refspec_string);
      refspec = REFSPEC (refspec_new (self, refspec_string));
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
_sysroot_load_refspecs (Sysroot *self,
                        OstreeRepo *ot_repo)
{
  GHashTable *refs = NULL;
  GHashTableIter iter;
  GHashTableIter iter_old;
  GError *error = NULL;
  gpointer hashkey;
  gpointer value;

  // Add refspec interfaces
  if (!ostree_repo_list_refs(ot_repo,
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
    _sysroot_add_refspec (self, hashkey, ot_repo);

  g_hash_table_remove_all (refs);
  g_hash_table_unref (refs);
  g_debug ("finished refspecs");

  g_rw_lock_writer_unlock (&self->children_lock);

  g_clear_error (&error);
}


static gboolean
handle_get_ref_specs (RPMOSTreeSysroot *object,
                     GDBusMethodInvocation *invocation)
{

  Sysroot *self = SYSROOT (object);

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
handle_get_deployments (RPMOSTreeSysroot *object,
                        GDBusMethodInvocation *invocation,
                        GVariant *arg_options)
{
  Sysroot *self = SYSROOT (object);

  GPtrArray *deployments = NULL;
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
  g_ptr_array_unref (deployments);

  g_variant_builder_add_value (&tbuilder, g_variant_builder_end (&builder));
  variant = g_variant_builder_end (&tbuilder);

  // calling invoke directly to just pass the variant;
  g_dbus_method_invocation_return_value (invocation, variant);

  return TRUE;
}


static gboolean
handle_cancel_update (RPMOSTreeSysroot *object,
                      GDBusMethodInvocation *invocation)
{
  Sysroot *self = SYSROOT (object);
  g_debug ("Canceling tasks");
  g_signal_emit (self, sysroot_signals[CANCEL_TASKS], 0);
  rpmostree_sysroot_complete_cancel_update (object, invocation);
  return TRUE;
}


gboolean
sysroot_begin_update_operation (Sysroot *self,
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
    rpmostree_sysroot_set_update_running (RPMOSTREE_SYSROOT (self),
                                          type);
  return ret;
}


static void
emit_update_complete_after_refresh (Sysroot *self,
                                    gpointer user_data)
{
  gchar *message = user_data;
  rpmostree_sysroot_set_update_running (RPMOSTREE_SYSROOT (self),
                                        "");
  rpmostree_sysroot_emit_update_completed (RPMOSTREE_SYSROOT (self),
                                          TRUE,
                                          message);
  g_signal_handlers_disconnect_by_data (self, user_data);

}


void
sysroot_end_update_operation (Sysroot *self,
                              gboolean success,
                              const gchar *message,
                              gboolean wait_for_refresh)
{
  if (!wait_for_refresh)
    {
      rpmostree_sysroot_set_update_running (RPMOSTREE_SYSROOT (self),
                                            "");
      rpmostree_sysroot_emit_update_completed (RPMOSTREE_SYSROOT (self),
                                              success,
                                              g_strdup (message));
    }
  else
    {
      g_debug ("waiting for update complete signal");
      g_signal_connect_data (self, "interfaces-updated",
                             G_CALLBACK (emit_update_complete_after_refresh),
                             g_strdup (message), (GClosureNotify) g_free, 0);
      sysroot_ensure_refresh (self);
    }

  g_mutex_unlock (&self->update_lock);
}


static gboolean
handle_get_upgrade_ref_spec (RPMOSTreeSysroot *object,
                             GDBusMethodInvocation *invocation,
                             GVariant *arg_options)
{
  gs_unref_object RefSpec *current_refspec = NULL;
  gs_free gchar *osname = NULL;
  GError *error = NULL;

  Sysroot *self = SYSROOT (object);

  if (arg_options)
    g_variant_lookup (arg_options, "os", "s", &osname);

  current_refspec = _get_refspec_for_os (self, osname, &error);
  if (current_refspec)
    {
      gchar *dpath = NULL;
      g_object_get (current_refspec, "dbus-path", &dpath, NULL);
      rpmostree_sysroot_complete_get_upgrade_ref_spec (object,
                                                       invocation,
                                                       dpath);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, error);
    }

  g_clear_error (&error);
  return TRUE;
}


static void
add_ref_spec_callback (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  GTask *task = G_TASK (res);
  GError *error = NULL;
  GDBusMethodInvocation *invocation = user_data;
  Sysroot *sysroot = SYSROOT (source_object);

  if (!g_task_propagate_boolean (task, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
    }
  else
    {
      gchar *refspec = g_task_get_task_data (task); // freed with task
      gs_free gchar *dbus_path = sysroot_generate_sub_object_path (sysroot,
                                                           REFSPEC_DBUS_PATH_NAME,
                                                           refspec,
                                                           NULL);

      DelayedInvocation *di = delayed_invocation_new (invocation,
                                                     g_variant_new ("(o)", dbus_path));
      g_signal_connect_data (sysroot, "interfaces-updated",
                             G_CALLBACK (delayed_invocation_invoke), di,
                             (GClosureNotify) delayed_invocation_free, 0);
      sysroot_ensure_refresh (sysroot);
    }

  g_clear_error (&error);
  g_object_unref (task);
}


static gboolean
handle_add_ref_spec (RPMOSTreeSysroot *object,
                     GDBusMethodInvocation *invocation,
                     GVariant *arg_options,
                     const gchar *new_provided_refspec)
{

  Sysroot *self = SYSROOT (object);
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


void
sysroot_watch_client_if_needed (Sysroot *self,
                                GDBusConnection *conn,
                                const gchar *sender)
{
  if (!g_str_equal (self->path, SYSROOT_DEFAULT_PATH))
      daemon_watch_client (daemon_get (), conn, sender);
}


gboolean
sysroot_track_client_auth (GDBusInterfaceSkeleton *instance,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data)
{
  Sysroot *self = SYSROOT (user_data);
  gboolean ret = auth_check_root_or_access_denied (instance,
                                                   invocation,
                                                   user_data);

  if (ret)
    sysroot_watch_client_if_needed (self,
                                    g_dbus_method_invocation_get_connection (invocation),
                                    g_dbus_method_invocation_get_sender (invocation));
  return ret;
}

static gboolean
sysroot_load_internals (Sysroot *self,
                        OstreeSysroot **out_sysroot,
                        OstreeRepo **out_repo,
                        GError **out_error)
{
  gboolean ret = FALSE;
  OstreeSysroot *ot_sysroot = NULL;
  OstreeRepo *ot_repo = NULL;
  GError *error = NULL;

  if (!utils_load_sysroot_and_repo (self->path,
                                    self->cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    &error))
      goto out;

  g_debug ("loading deployments and refspecs");
  _sysroot_load_refspecs (self, ot_repo);
  _sysroot_load_deployments (self, ot_sysroot, ot_repo);
  ret = TRUE;

out:
  if (error == NULL)
    {
      gs_transfer_out_value (out_sysroot, &ot_sysroot);
      gs_transfer_out_value (out_repo, &ot_repo);
    }
  else
    {
      g_message ("error %s", error->message);
      gs_transfer_out_value (out_error, &error);
      if (ot_repo)
        g_object_unref (ot_repo);
      if (ot_sysroot)
        g_object_unref (ot_sysroot);
    }

  return ret;
}

static void
_do_reload_data (GTask         *task,
                 gpointer       object,
                 gpointer       data_ptr,
                 GCancellable  *cancellable)
{
  GError *error = NULL;
  Sysroot *self = SYSROOT (object);
  gint i;

  g_debug ("reloading");
  for (i = 0; i < 3; i++)
    {
      if (sysroot_load_internals (self, NULL, NULL, &error))
        break;
      else
        g_message ("Error refreshing sysroot data: %s", error->message);
    }

  // Call closures in callback queue
  g_clear_error (&error);
}


static void
_reload_callback (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
    Sysroot *self = SYSROOT (user_data);
    GTask *task = G_TASK (res);

    g_signal_emit (self, sysroot_signals[UPDATED], 0);
    g_object_unref (task);
}


static gboolean
_throttle_refresh (gpointer user_data)
{
  Sysroot *self = SYSROOT (user_data);
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
on_transaction_file (GFileMonitor *monitor,
                     GFile *file,
                     GFile *other_file,
                     GFileMonitorEvent event_type,
                     gpointer user_data)
{
  Sysroot *self = SYSROOT (user_data);
  if (event_type == G_FILE_MONITOR_EVENT_DELETED)
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


static void
sysroot_ensure_refresh (Sysroot *self)
{
  gboolean needs_run;
  g_rw_lock_reader_lock (&self->children_lock);
    needs_run = self->last_transaction_end == 0;
  g_rw_lock_reader_unlock (&self->children_lock);

  if (needs_run)
    _throttle_refresh (self);
}


static void
sysroot_constructed (GObject *object)
{
  Sysroot *self = SYSROOT (object);

  self->dbus_path = utils_generate_object_path ("/org/projectatomic/rpmostree1",
                                                "Sysroots", self->dbus_name, NULL);
  rpmostree_sysroot_set_sysroot_path (RPMOSTREE_SYSROOT (self),
                                      self->path);

  g_signal_connect (RPMOSTREE_SYSROOT(object), "g-authorize-method",
                    G_CALLBACK (sysroot_track_client_auth), self);
}


static void
sysroot_get_property (GObject *object,
                      guint prop_id,
                      GValue *value,
                      GParamSpec *pspec)
{
  Sysroot *self = SYSROOT (object);

  switch (prop_id)
    {
      case PROP_DBUS_PATH:
        g_value_set_string (value, self->dbus_path);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
sysroot_set_property (GObject *object,
                      guint prop_id,
                      const GValue *value,
                      GParamSpec *pspec)
{
  Sysroot *self = SYSROOT (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_assert (self->path == NULL);
      self->path = g_value_dup_string (value);
      break;
    case PROP_DBUS_NAME:
      g_assert (self->dbus_name == NULL);
      self->dbus_name = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
sysroot_class_init (SysrootClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = sysroot_dispose;
  gobject_class->finalize     = sysroot_finalize;
  gobject_class->constructed  = sysroot_constructed;
  gobject_class->set_property = sysroot_set_property;
  gobject_class->get_property = sysroot_get_property;

  /**
   * Sysroot:path:
   *
   * The Sysroot path
   */
  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        NULL,
                                                        NULL,
                                                        NULL,
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * Sysroot:dbus_name:
   *
   * Name for this sysroot on the bus
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DBUS_NAME,
                                   g_param_spec_string ("dbus-name",
                                                        NULL,
                                                        NULL,
                                                        NULL,
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * Sysroot:dbus_path:
   *
   * Name for this sysroot on the bus
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DBUS_PATH,
                                   g_param_spec_string ("dbus-path",
                                                        NULL,
                                                        NULL,
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  sysroot_signals[CANCEL_TASKS] = g_signal_new ("cancel-tasks",
                                                RPM_OSTREE_TYPE_DAEMON_SYSROOT,
                                                G_SIGNAL_RUN_LAST,
                                                0, NULL, NULL,
                                                g_cclosure_marshal_generic,
                                                G_TYPE_NONE, 0);

  sysroot_signals[UPDATED] = g_signal_new ("interfaces-updated",
                                           RPM_OSTREE_TYPE_DAEMON_SYSROOT,
                                           G_SIGNAL_RUN_LAST,
                                           0, NULL, NULL,
                                           g_cclosure_marshal_generic,
                                           G_TYPE_NONE, 0);
}

/**
 * sysroot_new:
 * @path: Filesystem path.
 *
 * Creates a new #Sysroot instance.
 *
 * Returns: A new #RPMOSTreeSysroot. Free with g_object_unref().
 */
static RPMOSTreeSysroot *
sysroot_new (const gchar *path, const gchar *dbus_name)
{
  return RPMOSTREE_SYSROOT (g_object_new (RPM_OSTREE_TYPE_DAEMON_SYSROOT,
                                          "path", path,
                                          "dbus-name", dbus_name,
                                          NULL));
}

/**
 * sysroot_get_path :
 *
 * Returns: The filesystem path for sysroot.
 * Do not free owned by sysroot
 */
gchar *
sysroot_get_path (Sysroot *self)
{
  return self->path;
}

/**
 * sysroot_publish :
 * @path: Sysroot path
 * @dbus_name: Name for this path on the dbus
 * @error: **error
 *
 * Creates and publishes a new sysroot instance.
 * Ensures that sysroot and repo are valid
 *
 * Returns: A pointer to a Sysroot instance.
 */

gboolean
sysroot_publish_new (const gchar *path,
                     const gchar *dbus_name,
                     Sysroot **out_sysroot,
                     GError **error)
{
  Sysroot *sysroot = NULL;
  gboolean ret = FALSE;
  gs_unref_object OstreeRepo *ot_repo = NULL;

  GFile *repo_file = NULL;
  GFile *transaction_file = NULL;

  g_debug ("Creating new sysroot");
  sysroot = SYSROOT (sysroot_new (path, dbus_name));

  if (!sysroot_load_internals (sysroot, NULL, &ot_repo, error))
    goto out;

  repo_file = ostree_repo_get_path (ot_repo);
  transaction_file = g_file_get_child (repo_file, "transaction");
  sysroot->monitor = g_file_monitor (transaction_file, 0, NULL, error);

  if (sysroot->monitor == NULL)
    goto out;

  sysroot->sig_changed = g_signal_connect (sysroot->monitor,
                                           "changed",
                                           G_CALLBACK (on_transaction_file),
                                           sysroot);

  if (sysroot->dbus_path != NULL)
    {
      daemon_publish (daemon_get (), sysroot->dbus_path, FALSE, sysroot);
      gs_transfer_out_value (out_sysroot, &sysroot);
      ret = TRUE;
    }
  else
    {
      g_set_error (error, RPM_OSTREED_ERROR,
                   RPM_OSTREED_ERROR_FAILED,
                   "Couldn't generate object path for %s",
                   dbus_name);
    }

out:
  if (transaction_file)
    g_object_unref (transaction_file);
  return ret;
}


static void
sysroot_iface_init (RPMOSTreeSysrootIface *iface)
{
  iface->handle_get_ref_specs = handle_get_ref_specs;
  iface->handle_get_upgrade_ref_spec = handle_get_upgrade_ref_spec;
  iface->handle_get_deployments = handle_get_deployments;
  iface->handle_add_ref_spec = handle_add_ref_spec;
  iface->handle_cancel_update = handle_cancel_update;
}

/* ---------------------------------------------------------------------------------------------------- */
