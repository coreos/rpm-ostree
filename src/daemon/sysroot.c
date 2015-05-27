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

#include "types.h"
#include "daemon.h"
#include "sysroot.h"
#include "os.h"
#include "utils.h"
#include "deployment-utils.h"
#include "auth.h"
#include "errors.h"

#include "libgsystem.h"
#include <libglnx.h>

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

  gchar *sysroot_path;

  GHashTable *os_interfaces;
  GRWLock children_lock;

  GFileMonitor *monitor;
  guint sig_changed;
  guint64 last_monitor_event;
};

struct _SysrootClass
{
  RPMOSTreeSysrootSkeletonClass parent_class;
};

enum
{
  PROP_0,
  PROP_PATH,
};

enum {
  UPDATED,
  NUM_SIGNALS
};

static guint64 UPDATED_THROTTLE_SECONDS = 2;
static guint sysroot_signals[NUM_SIGNALS] = { 0, };

static void sysroot_iface_init (RPMOSTreeSysrootIface *iface);


G_DEFINE_TYPE_WITH_CODE (Sysroot, sysroot, RPMOSTREE_TYPE_SYSROOT_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_SYSROOT,
                                                sysroot_iface_init));

static Sysroot *_sysroot_instance;

/* ---------------------------------------------------------------------------------------------------- */

static GFile *
_build_file (const char *first, ...)
{
  va_list args;
  const gchar *arg;
  gs_free gchar *path = NULL;
  g_autoptr(GPtrArray) parts = NULL;

  va_start (args, first);
  parts = g_ptr_array_new ();
  arg = first;
  while (arg != NULL)
    {
      g_ptr_array_add (parts, (char*)arg);
      arg = va_arg (args, const char *);
    }
  va_end (args);

  g_ptr_array_add (parts, NULL);
  path = g_build_filenamev ((gchar**)parts->pdata);
  return g_file_new_for_path (path);
}

static gboolean
handle_create_osname (RPMOSTreeSysroot *object,
                     GDBusMethodInvocation *invocation,
                     const gchar *osname)
{
  gs_unref_object OstreeSysroot *ot_sysroot = NULL;
  g_autoptr(GFile) dir = NULL;
  gs_free gchar *deploy_dir = NULL;
  gs_free gchar *base_name = NULL;

  GError *error = NULL;
  gs_free gchar *dbus_path = NULL;

  Sysroot *self = SYSROOT (object);

  if (!utils_load_sysroot_and_repo (self->sysroot_path,
                                    self->cancellable,
                                    &ot_sysroot,
                                    NULL,
                                    &error))
      goto out;

  if (!ostree_sysroot_ensure_initialized (ot_sysroot,
                                          self->cancellable,
                                          &error))
    goto out;

  base_name = g_path_get_basename(osname);
  if (g_strcmp0 (base_name, osname) != 0)
    {
      g_set_error_literal (&error,
                           RPM_OSTREED_ERROR,
                           RPM_OSTREED_ERROR_FAILED,
                           "Invalid osname");
      goto out;
    }

  // TODO: The operations here are copied from ostree init-os
  // command, should be refactored into shared code
  deploy_dir = g_build_filename (self->sysroot_path,
                                 "ostree", "deploy", osname, NULL);

  /* Ensure core subdirectories of /var exist, since we need them for
   * dracut generation, and the host will want them too.
   */
  g_clear_object (&dir);
  dir = _build_file (deploy_dir, "var", "tmp", NULL);
  if (!gs_file_ensure_directory (dir, TRUE, self->cancellable, &error))
    goto out;
  if (chmod (gs_file_get_path_cached (dir), 01777) < 0)
    {
      gs_set_error_from_errno (&error, errno);
      goto out;
    }

  g_clear_object (&dir);
  dir = _build_file (deploy_dir, "var", "lib", NULL);
  if (!gs_file_ensure_directory (dir, TRUE, self->cancellable, &error))
    goto out;

  g_clear_object (&dir);
  dir = _build_file (deploy_dir, "var", "run", NULL);
  if (!g_file_test (gs_file_get_path_cached (dir), G_FILE_TEST_IS_SYMLINK))
    {
      if (symlink ("../run", gs_file_get_path_cached (dir)) < 0)
        {
          gs_set_error_from_errno (&error, errno);
          goto out;
        }
    }

  dir = _build_file (deploy_dir, "var", "lock", NULL);
  if (!g_file_test (gs_file_get_path_cached (dir), G_FILE_TEST_IS_SYMLINK))
    {
      if (symlink ("../run/lock", gs_file_get_path_cached (dir)) < 0)
        {
          gs_set_error_from_errno (&error, errno);
          goto out;
        }
    }

  dbus_path = utils_generate_object_path (BASE_DBUS_PATH,
                                          osname, NULL);
  sysroot_ensure_refresh (SYSROOT (self));
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
  Sysroot *self = SYSROOT (object);
  glnx_unref_object GDBusInterfaceSkeleton *os_interface = NULL;

  g_rw_lock_reader_lock (&self->children_lock);

  os_interface = g_hash_table_lookup (self->os_interfaces, arg_name);
  if (os_interface != NULL)
    g_object_ref (os_interface);

  g_rw_lock_reader_unlock (&self->children_lock);

  if (os_interface != NULL)
    {
      const char *object_path;
      object_path = g_dbus_interface_skeleton_get_object_path (os_interface);
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

/* ---------------------------------------------------------------------------------------------------- */
static void
sysroot_dispose (GObject *object)
{
  Sysroot *self = SYSROOT (object);
  GHashTableIter iter;
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

  /* Tracked os paths are responsible to unpublish themselves */
  g_hash_table_iter_init (&iter, self->os_interfaces);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    g_object_run_dispose (value);
  g_hash_table_remove_all (self->os_interfaces);

  g_rw_lock_writer_unlock (&self->children_lock);

  G_OBJECT_CLASS (sysroot_parent_class)->dispose (object);
}

static void
sysroot_finalize (GObject *object)
{
  Sysroot *self = SYSROOT (object);
  _sysroot_instance = NULL;

  g_free (self->sysroot_path);

  g_hash_table_unref (self->os_interfaces);

  g_rw_lock_clear (&self->children_lock);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->monitor);

  G_OBJECT_CLASS (sysroot_parent_class)->finalize (object);
}

static void
sysroot_init (Sysroot *self)
{
  g_assert (_sysroot_instance == NULL);
  _sysroot_instance = self;

  self->cancellable = g_cancellable_new ();

  self->os_interfaces = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
  (GDestroyNotify) g_object_unref);

  g_rw_lock_init (&self->children_lock);

  self->monitor = NULL;
  self->sig_changed = 0;
  self->last_monitor_event = 0;
}

static void
sysroot_constructed (GObject *object)
{
  Sysroot *self = SYSROOT (object);
  g_signal_connect (RPMOSTREE_SYSROOT(self), "g-authorize-method",
                    G_CALLBACK (auth_check_root_or_access_denied), NULL);
  G_OBJECT_CLASS (sysroot_parent_class)->constructed (object);
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
      g_assert (self->sysroot_path == NULL);
      self->sysroot_path = g_value_dup_string (value);
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

  /**
   * Sysroot:sysroot_path:
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

  sysroot_signals[UPDATED] = g_signal_new ("sysroot-updated",
                                           TYPE_SYSROOT,
                                           G_SIGNAL_RUN_LAST,
                                           0, NULL, NULL,
                                           g_cclosure_marshal_generic,
                                           G_TYPE_NONE, 0);
}


static gboolean
sysroot_populate_deployments (Sysroot *self,
                              OstreeSysroot *ot_sysroot,
                              OstreeRepo *ot_repo)
{
  gs_unref_object OstreeDeployment *booted = NULL;
  g_autoptr (GPtrArray) deployments = NULL;

  GVariantBuilder builder;
  guint i;
  gboolean ret = FALSE;

  g_debug ("loading deployments");
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ssisstsav)"));

  // Add deployment interfaces
  deployments = ostree_sysroot_get_deployments (ot_sysroot);

  if (deployments == NULL)
    goto out;

  for (i=0; i<deployments->len; i++)
    {
      g_variant_builder_add_value (&builder,
                                   deployment_generate_variant (deployments->pdata[i],
                                                                ot_repo));
    }

  booted = ostree_sysroot_get_booted_deployment (ot_sysroot);
  if (booted)
    {
      const gchar *os = ostree_deployment_get_osname (booted);
      gs_free gchar *path = utils_generate_object_path (BASE_DBUS_PATH,
                                                        os, NULL);
      rpmostree_sysroot_set_booted (RPMOSTREE_SYSROOT (self), path);
    }
  else
    {
      rpmostree_sysroot_set_booted (RPMOSTREE_SYSROOT (self), "/");
    }

  g_debug ("we think we are here");
  rpmostree_sysroot_set_deployments (RPMOSTREE_SYSROOT (self),
                                     g_variant_builder_end (&builder));
  g_debug ("finished deployments");

out:
  return ret;
}
static gboolean
sysroot_load_internals (Sysroot *self,
                        OstreeSysroot **out_sysroot,
                        OstreeRepo **out_repo,
                        GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object OstreeSysroot *ot_sysroot = NULL;
  gs_unref_object OstreeRepo *ot_repo = NULL;
  g_autoptr (GHashTable) seen = NULL;
  gs_free gchar *os_path = NULL;
  GDir *os_dir = NULL;

  GHashTableIter iter;
  gpointer hashkey;
  gpointer value;

  g_rw_lock_writer_lock (&self->children_lock);
  if (!utils_load_sysroot_and_repo (self->sysroot_path,
                                    self->cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    error))
      goto out;

  os_path = g_build_filename (self->sysroot_path, "ostree",
                              "deploy", NULL);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  os_dir = g_dir_open (os_path, 0, error);
  if (!os_dir)
    goto out;

  while (TRUE)
    {
      const gchar *os = g_dir_read_name (os_dir);
      gs_free gchar *full_path = NULL;

      if (os == NULL) {
        if (errno && errno != ENOENT)
          {
            glnx_set_error_from_errno (error);
            goto out;
          }
        break;
      }

      g_hash_table_add (seen, g_strdup (os));
      // If we've already seen it, continue
      if (g_hash_table_contains (self->os_interfaces, os))
        continue;

      full_path = g_build_filename(os_path, os, NULL);
      if (g_file_test (full_path, G_FILE_TEST_IS_DIR))
        {
          gs_free gchar *path = utils_generate_object_path (BASE_DBUS_PATH,
                                                            os, NULL);
          //TODO: stub for real OS implementation
          RPMOSTreeOS *obj = osstub_new (ot_sysroot, os);
          g_hash_table_insert(self->os_interfaces,
                              g_strdup (os),
                              obj);
          daemon_publish (daemon_get (), path, FALSE, obj);
        }
    }

  // Remove dead os paths
  g_hash_table_iter_init (&iter, self->os_interfaces);
  while (g_hash_table_iter_next (&iter, &hashkey, &value))
    {
      if (!g_hash_table_contains (seen, hashkey))
        {
          //TODO: stub for real implementation
          gs_free gchar *path = utils_generate_object_path (BASE_DBUS_PATH,
                                                            hashkey, NULL);
          daemon_unpublish (daemon_get (),
                            path,
                            G_OBJECT (value));
          g_object_run_dispose (G_OBJECT (value));
          g_hash_table_iter_remove (&iter);
        }
    }

  // update deployments
  sysroot_populate_deployments (self, ot_sysroot, ot_repo);

  gs_transfer_out_value (out_sysroot, &ot_sysroot);
  if (out_repo != NULL)
    *out_repo = g_object_ref (ot_repo);

  ret = TRUE;

out:
  g_rw_lock_writer_unlock (&self->children_lock);

  if (os_dir)
    g_dir_close(os_dir);

  return ret;
}


static gboolean _throttle_refresh (gpointer user_data);

static void
_do_reload_data (GTask         *task,
                 gpointer       object,
                 gpointer       data_ptr,
                 GCancellable  *cancellable)
{
  GError *error = NULL;
  Sysroot *self = SYSROOT (object);

  g_debug ("reloading");

  // this was valid once, make sure it is tried again
  // TODO, should we bail at some point?
  if (!sysroot_load_internals (self, NULL, NULL, &error)) {
      g_message ("Error refreshing sysroot data: %s", error->message);
      g_timeout_add_seconds (UPDATED_THROTTLE_SECONDS,
                             _throttle_refresh,
                             self);
  }
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

  // Only run the update if there isn't another one pending.
  g_rw_lock_writer_lock (&self->children_lock);
  if ((g_get_monotonic_time () - self->last_monitor_event) >
      UPDATED_THROTTLE_SECONDS * G_USEC_PER_SEC)
    {
      GTask *task = g_task_new (self, self->cancellable,
                                _reload_callback, self);
      g_task_set_return_on_cancel (task, TRUE);
      self->last_monitor_event = 0;
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
  Sysroot *self = SYSROOT (user_data);
  if (event_type == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
    {
      g_rw_lock_writer_lock (&self->children_lock);
      if (self->last_monitor_event == 0)
        g_timeout_add_seconds (UPDATED_THROTTLE_SECONDS,
                               _throttle_refresh,
                               user_data);

      self->last_monitor_event = g_get_monotonic_time ();
      g_rw_lock_writer_unlock (&self->children_lock);
    }
}



static void
sysroot_iface_init (RPMOSTreeSysrootIface *iface)
{
  iface->handle_create_osname = handle_create_osname;
  iface->handle_get_os = handle_get_os;
}


/**
 * sysroot_ensure_refresh:
 *
 * Ensures that the sysroot will reload it's
 * internal data.
 */

void
sysroot_ensure_refresh (Sysroot *self)
{
  gboolean needs_run;
  g_rw_lock_reader_lock (&self->children_lock);
    needs_run = self->last_monitor_event == 0;
  g_rw_lock_reader_unlock (&self->children_lock);

  if (needs_run)
    _throttle_refresh (self);
}

/**
 * sysroot_populate :
 *
 * loads internals and starts monitoring
 *
 * Returns: True on success
 */
gboolean
sysroot_populate (Sysroot *self,
                  GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object OstreeRepo *ot_repo = NULL;
  g_return_val_if_fail (self != NULL, FALSE);

  if (!sysroot_load_internals (self, NULL, &ot_repo, error))
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
 * sysroot_get_sysroot_path :
 *
 * Returns: The filesystem path for sysroot.
 * Do not free owned by sysroot
 */
gchar *
sysroot_get_sysroot_path (Sysroot *self)
{
  return self->sysroot_path;
}


/**
 * sysroot_get:
 *
 * Returns: (transfer none): The singleton #Sysroot instance
 */
Sysroot *
sysroot_get (void)
{
  g_assert (_sysroot_instance);
  return _sysroot_instance;
}



/* ---------------------------------------------------------------------------------------------------- */
