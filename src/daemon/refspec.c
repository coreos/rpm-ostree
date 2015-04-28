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

#include "refspec.h"
#include "manager.h"
#include "daemon.h"
#include "utils.h"
#include "auth.h"
#include "errors.h"

#include "libgsystem.h"


/**
 * @title: RefSpec
 * @short_description: Implementation of #RPMOSTreeRefSpec
 *
 * This type provides an implementation of the #RPMOSTreeRefSpec interface.
 */

typedef struct _RefSpecClass RefSpecClass;
struct _RefSpec
{
  RPMOSTreeRefSpecSkeleton parent_instance;

  gchar *id;
  gchar *dbus_path;

  volatile gint updating;
  GCancellable *cancellable;
};

struct _RefSpecClass
{
  RPMOSTreeRefSpecSkeletonClass parent_class;
};

enum
{
  PROP_0,
  PROP_DBUS_PATH,
  PROP_ID,
};

static void refspec_iface_init (RPMOSTreeRefSpecIface *iface);

G_DEFINE_TYPE_WITH_CODE (RefSpec, refspec, RPMOSTREE_TYPE_REF_SPEC_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_REF_SPEC,
                                                refspec_iface_init));

/* ---------------------------------------------------------------------------------------------------- */


static void
_pull_progress (OstreeAsyncProgress *progress,
                gpointer user_data)
{
  gs_free char *status = NULL;
  RefSpec *refspec = REFSPEC (user_data);
  status = ostree_async_progress_get_status (progress);
  if (status)
    {
      rpmostree_ref_spec_emit_progress_message (RPMOSTREE_REF_SPEC (refspec),
                                                g_strdup (status));
    }
  else
    {
      guint64 start_time = ostree_async_progress_get_uint64 (progress, "start-time");
      guint64 elapsed_secs = 0;

      guint outstanding_fetches = ostree_async_progress_get_uint (progress, "outstanding-fetches");
      guint outstanding_writes = ostree_async_progress_get_uint (progress, "outstanding-writes");

      guint n_scanned_metadata = ostree_async_progress_get_uint (progress, "scanned-metadata");
      guint metadata_fetched = ostree_async_progress_get_uint (progress, "metadata-fetched");
      guint outstanding_metadata_fetches = ostree_async_progress_get_uint (progress, "outstanding-metadata-fetches");

      guint total_delta_parts = ostree_async_progress_get_uint (progress, "total-delta-parts");
      guint fetched_delta_parts = ostree_async_progress_get_uint (progress, "fetched-delta-parts");
      guint total_delta_superblocks = ostree_async_progress_get_uint (progress, "total-delta-superblocks");
      guint64 total_delta_part_size = ostree_async_progress_get_uint64 (progress, "total-delta-part-size");

      guint fetched = ostree_async_progress_get_uint (progress, "fetched");
      guint requested = ostree_async_progress_get_uint (progress, "requested");

      guint64 bytes_sec = 0;
      guint64 bytes_transferred = ostree_async_progress_get_uint64 (progress, "bytes-transferred");

      if (start_time)
        {
          guint64 elapsed_secs = (g_get_monotonic_time () - start_time) / G_USEC_PER_SEC;
          if (elapsed_secs)
            bytes_sec = bytes_transferred / elapsed_secs;
        }

      rpmostree_ref_spec_emit_progress_data (RPMOSTREE_REF_SPEC(refspec),
                                             g_variant_new("(tt)",
                                                           start_time,
                                                           elapsed_secs),
                                             g_variant_new("(uu)",
                                                           outstanding_fetches,
                                                           outstanding_writes),
                                             g_variant_new("(uuu)",
                                                           n_scanned_metadata,
                                                           metadata_fetched,
                                                           outstanding_metadata_fetches),
                                             g_variant_new("(uuut)",
                                                           total_delta_parts,
                                                           fetched_delta_parts,
                                                           total_delta_superblocks,
                                                           total_delta_part_size),
                                             g_variant_new("(uu)",
                                                           fetched,
                                                           requested),
                                             g_variant_new("(tt)",
                                                           bytes_transferred,
                                                           bytes_sec));
    }
}


static void
_do_upgrade_in_thread (GTask *task,
                       gpointer object,
                       gpointer data_ptr,
                       GCancellable *cancellable)
{
  GError *error = NULL;
  GVariant *options = data_ptr;

  gs_unref_object OstreeSysroot *ot_sysroot = NULL;
  gs_unref_object OstreeRepo *ot_repo = NULL;
  gs_unref_object OstreeSysrootUpgrader *upgrader = NULL;
  gs_unref_object OstreeAsyncProgress *progress = NULL;

  gs_unref_keyfile GKeyFile *old_origin = NULL;
  gs_unref_keyfile GKeyFile *new_origin = NULL;

  gs_free gchar *osname = NULL;
  gs_free gchar *origin_refspec = NULL;
  gs_free gchar *origin_remote = NULL;
  gs_free gchar *origin_ref = NULL;
  gs_free gchar *new_refspec = NULL;
  gs_free gchar *remote = NULL;
  gs_free gchar *ref = NULL;

  gboolean skip_purge = FALSE;
  OstreeSysrootUpgraderPullFlags upgraderpullflags = 0;
  gboolean allow_downgrade = FALSE;
  gboolean changed = FALSE;
  gboolean ret = FALSE;

  RefSpec *self = REFSPEC (object);

  // libostree iterates and calls quit on main loop
  // so we need to run in our own context.
  GMainContext *m_context = g_main_context_new ();
  g_main_context_push_thread_default (m_context);

  if (!utils_load_sysroot_and_repo (manager_get_sysroot_path ( manager_get ()),
                                    cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    &error))
    goto out;

  upgrader = ostree_sysroot_upgrader_new_for_os (ot_sysroot,
                                                 osname,
                                                 cancellable,
                                                 &error);
  if (!upgrader)
    goto out;

  if (options != NULL)
    {
      g_variant_lookup (options, "os", "s", &osname);
      g_variant_lookup (options, "skip-purge", "b", &skip_purge);
      g_variant_lookup (options, "allow-downgrade", "b", &allow_downgrade);
    }

  old_origin = ostree_sysroot_upgrader_get_origin (upgrader);
  origin_refspec = g_key_file_get_string (old_origin, "origin", "refspec", NULL);
  if (origin_refspec)
    {
      if (!ostree_parse_refspec (origin_refspec, &origin_remote, &origin_ref, &error))
        goto out;
    }

  remote = rpmostree_ref_spec_dup_remote_name (RPMOSTREE_REF_SPEC(self));
  ref = rpmostree_ref_spec_dup_ref (RPMOSTREE_REF_SPEC(self));
  new_refspec = g_strconcat (remote, ":", ref, NULL);

  if (g_strcmp0 (origin_refspec, new_refspec) == 0)
    {
      // If origin and ref are the same never purge
      skip_purge = TRUE;
    }
  else
    {
      // We are rebasing
      rpmostree_manager_set_update_running (RPMOSTREE_MANAGER (manager_get ()),
                                            "rebase");

      //downgrade is always allowed for rebasing
      allow_downgrade = TRUE;
      new_origin = ostree_sysroot_origin_new_from_refspec (ot_sysroot,
                                                           new_refspec);
      if (!ostree_sysroot_upgrader_set_origin (upgrader, new_origin,
                                               cancellable, &error))
        goto out;
    }

  g_debug ("update starting");

  if (allow_downgrade)
    upgraderpullflags |= OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER;

  progress = ostree_async_progress_new_and_connect (_pull_progress, self);
  if (!ostree_sysroot_upgrader_pull (upgrader, 0, upgraderpullflags,
                                     progress, &changed, cancellable, &error))
    goto out;

  if (changed)
    {
      if (!ostree_sysroot_upgrader_deploy (upgrader, cancellable, &error))
        goto out;

      if (!skip_purge && origin_refspec != NULL)
        {
          if (!ostree_repo_prepare_transaction (ot_repo, NULL, cancellable, &error))
            goto out;

          ostree_repo_transaction_set_ref (ot_repo, origin_remote,
                                           origin_ref, NULL);

          if (!ostree_repo_commit_transaction (ot_repo, NULL,
                                               cancellable, &error))
            goto out;
        }
        ret = TRUE;
    }
out:


  // Clean up context
  g_main_context_pop_thread_default (m_context);
  g_main_context_unref (m_context);

  if (error == NULL)
      g_task_return_boolean (task, ret);
  else
      g_task_return_error (task, error);
}


static gboolean
pull_dir (const gchar *dir,
          const gchar *remote,
          const gchar *ref,
          OstreeAsyncProgress *progress,
          GCancellable *cancellable,
          GError **error)
{
  gboolean ret = FALSE;

  gs_free gchar *refs_to_fetch[] = { NULL, NULL };
  gs_unref_object OstreeSysroot *ot_sysroot = NULL;
  gs_unref_object OstreeRepo *ot_repo = NULL;
  GMainContext *m_context = NULL;
  refs_to_fetch[0] = g_strdup (ref);

  g_debug ("pulling dir %s", dir);
  // libostree iterates and calls quit on main loop
  // so we need to run in our own context.
  m_context = g_main_context_new ();
  g_main_context_push_thread_default (m_context);

  if (!utils_load_sysroot_and_repo (manager_get_sysroot_path ( manager_get ()),
                                    cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    error))
    goto out;

  ret = ostree_repo_pull_one_dir (ot_repo, remote,
                                  dir, refs_to_fetch,
                                  0, progress,
                                  cancellable, error);

  // Clean up context
  g_main_context_pop_thread_default (m_context);
  g_main_context_unref (m_context);

out:
  return ret;
}


static void
_do_pull_nodata (GTask *task,
                  gpointer object,
                  gpointer data_ptr,
                  GCancellable *cancellable)
{
  GError *error = NULL;

  gs_free gchar *ref = NULL;
  gs_free gchar *remote = NULL;

  gchar *refspec = data_ptr;

  if (!ostree_parse_refspec (refspec, &remote, &ref, &error))
    goto out;

  pull_dir ("/nonexistent", remote, ref,
            NULL, cancellable, &error);

out:
  if (error == NULL)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}


static void
_do_pull_rpm (GTask *task,
              gpointer object,
              gpointer data_ptr,
              GCancellable *cancellable)
{
  GError *error = NULL;

  gs_unref_object OstreeAsyncProgress *progress = NULL;
  gs_free gchar *ref = NULL;
  gs_free gchar *remote = NULL;

  RefSpec *self = REFSPEC (object);

  g_debug ("Pull starting");

  remote = rpmostree_ref_spec_dup_remote_name (RPMOSTREE_REF_SPEC(self));
  ref = rpmostree_ref_spec_dup_ref (RPMOSTREE_REF_SPEC(self));

  progress = ostree_async_progress_new_and_connect (_pull_progress,
                                                    object);
  pull_dir ("/usr/share/rpm", remote, ref,
            progress, cancellable, &error);

  if (error == NULL)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}


static void
launch_thread (RefSpec *self,
               GAsyncReadyCallback callback,
               GTaskThreadFunc task_func,
               gpointer task_data,
               GDestroyNotify destroy_func)
{
  GTask *task = NULL;

  task = daemon_get_new_task (daemon_get (),
                              self,
                              self->cancellable,
                              callback,
                              self);

  if (task_data != NULL)
    g_task_set_task_data (task, task_data, destroy_func);

  g_atomic_int_set (&self->updating, TRUE);

  g_task_run_in_thread (task, task_func);
}


static void
_pull_rpm_callback (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
  RefSpec *self = REFSPEC (user_data);
  GTask *task = G_TASK (res);
  GError *error = NULL;
  gchar *message = NULL;
  gboolean success = FALSE;

  g_atomic_int_set (&self->updating, FALSE);

  success = g_task_propagate_boolean (task, &error);
  if (!success)
    message = g_strdup (error->message);
  else
    message = g_strdup ("Pull Complete.");

  manager_end_update_operation (manager_get (), success, message, FALSE);

  g_clear_error (&error);
  g_free (message);
  g_object_unref (task);
}


static void
_update_callback (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
  GError *error = NULL;

  RefSpec *self = REFSPEC (user_data);
  gs_unref_object GTask *task = G_TASK (res);
  gs_free gchar *message = NULL;
  gboolean success = FALSE;
  gboolean result = FALSE;

  success = g_task_propagate_boolean (task, &error);

  if (error != NULL)
    {
      message = g_strdup (error->message);
      g_message ("Error running upgrade: %s", error->message);
    }
  else
    {
      result = TRUE;
      if (success)
        message = g_strdup ("Upgrade prepared for next boot.");
      else
        message = g_strdup ("No upgrade available.");
    }

  manager_end_update_operation (manager_get (), result, message, success);

  g_clear_error (&error);
}


static gboolean
_refspec_ensure_remote (RefSpec *self,
                        GDBusMethodInvocation *invocation)
{
  gs_free gchar *remote = NULL;
  gboolean ret = FALSE;

  remote = rpmostree_ref_spec_dup_remote_name (RPMOSTREE_REF_SPEC (self));

  if (remote != NULL)
    ret = TRUE;
  else
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     RPM_OSTREED_ERROR,
                                                     RPM_OSTREED_ERROR_NO_REMOTE,
                                                     "Can't pull from a RefSpec with no remote");

  return ret;
}

static gboolean
handle_pull_rpm_db (RPMOSTreeRefSpec *object,
                    GDBusMethodInvocation *invocation)
{
  RefSpec *self = REFSPEC (object);
  if (!_refspec_ensure_remote (self, invocation))
    return TRUE;

  if (manager_begin_update_operation(manager_get (),
                                     invocation,
                                     "rpm-pull"))
    {
      rpmostree_ref_spec_complete_pull_rpm_db (object, invocation);
      launch_thread (self, _pull_rpm_callback,
                     _do_pull_rpm, NULL, NULL);
    }

  return TRUE;
}


static gboolean
handle_deploy (RPMOSTreeRefSpec *object,
               GDBusMethodInvocation *invocation,
               GVariant *arg_options)
{
  RefSpec *self = REFSPEC (object);

  if (!_refspec_ensure_remote (self, invocation))
    return TRUE;

  if (manager_begin_update_operation(manager_get (),
                                     invocation,
                                     "upgrade"))
    {
      rpmostree_ref_spec_complete_deploy (object, invocation);
      launch_thread (self, _update_callback, _do_upgrade_in_thread,
                     g_variant_ref (arg_options),
                     (GDestroyNotify) g_variant_unref);
    }

  return TRUE;
}


static gboolean
handle_get_rpm_diff (RPMOSTreeRefSpec *object,
                     GDBusMethodInvocation *invocation)
{
  // TODO: Get RPM diff
  GVariant *value = NULL;
  value = g_variant_new ("a(sya{sv})", NULL);
  rpmostree_ref_spec_complete_get_rpm_diff (object, invocation, value);
  return TRUE;
}


/* ---------------------------------------------------------------------------------------------------- */

gboolean
refspec_populate (RefSpec *refspec,
                  const gchar *refspec_string,
                  OstreeRepo *repo,
                  gboolean publish)
{
  gs_free gchar *ref = NULL;
  gs_free gchar *remote_name = NULL;
  gs_free gchar *head = NULL;

  GError *error = NULL;
  gboolean ret;

  ret = FALSE;
  if (!ostree_parse_refspec (refspec_string, &remote_name, &ref, &error))
    {
      g_warning ("error parsing refspec %s: %s",
                refspec->id, error->message);
      goto out;
    }

  if (repo != NULL &&
      !ostree_repo_resolve_rev (repo, refspec_string, FALSE, &head, &error))
    {
        g_warning ("error couldn't get head for refspec %s: %s",
                  refspec->id, error->message);
        goto out;
    }

  rpmostree_ref_spec_set_head (RPMOSTREE_REF_SPEC (refspec), head);
  rpmostree_ref_spec_set_remote_name (RPMOSTREE_REF_SPEC (refspec), remote_name);
  rpmostree_ref_spec_set_ref (RPMOSTREE_REF_SPEC (refspec), ref);

  ret = TRUE;
  if (publish)
    {
        daemon_publish (daemon_get (), refspec->dbus_path, FALSE, refspec);
        g_debug ("refspec %s published", refspec->id);
    }
  else
    {
        g_debug ("refspec %s updated", refspec->id);
    }

out:
  g_clear_error (&error);
  return ret;
}


static void
cancel_tasks (RefSpec *self)
{
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();
}


static void
refspec_dispose (GObject *object)
{
  RefSpec *self = REFSPEC (object);

  g_cancellable_cancel (self->cancellable);

  daemon_unpublish (daemon_get (), self->dbus_path, self);
  g_signal_handlers_disconnect_by_data (manager_get (), self);
  G_OBJECT_CLASS (refspec_parent_class)->dispose (object);
}

static void
refspec_finalize (GObject *object)
{
  RefSpec *refspec = REFSPEC (object);
  g_free (refspec->id);
  g_free (refspec->dbus_path);
  g_clear_object (&refspec->cancellable);
  G_OBJECT_CLASS (refspec_parent_class)->finalize (object);
}

static void
refspec_get_property (GObject *object,
                      guint prop_id,
                      GValue *value,
                      GParamSpec *pspec)
{
  RefSpec *self = REFSPEC (object);

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
refspec_set_property (GObject *object,
                      guint prop_id,
                      const GValue *value,
                      GParamSpec *pspec)
{
  RefSpec *refspec = REFSPEC (object);

  switch (prop_id)
    {
    case PROP_DBUS_PATH:
      g_assert (refspec->dbus_path == NULL);
      refspec->dbus_path = g_value_dup_string (value);
      break;
    case PROP_ID:
      g_assert (refspec->id == NULL);
      refspec->id = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
refspec_init (RefSpec *self)
{
  self->cancellable = g_cancellable_new ();
}

static void
refspec_constructed (GObject *object)
{
  RefSpec *self = REFSPEC (object);

  G_OBJECT_CLASS (refspec_parent_class)->constructed (object);

  g_signal_connect (RPMOSTREE_REF_SPEC(self), "g-authorize-method",
                    G_CALLBACK (auth_check_root_or_access_denied), NULL);
  g_signal_connect_swapped (manager_get (), "cancel-tasks",
                    G_CALLBACK (cancel_tasks), self);
}

static void
refspec_class_init (RefSpecClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose      = refspec_dispose;
  gobject_class->finalize     = refspec_finalize;
  gobject_class->constructed  = refspec_constructed;
  gobject_class->set_property = refspec_set_property;
  gobject_class->get_property = refspec_get_property;


  /**
   * RefSpec:id:
   *
   * The RefSpec string
   */
  g_object_class_install_property (gobject_class,
                                   PROP_ID,
                                   g_param_spec_string ("id",
                                                        NULL,
                                                        NULL,
                                                        NULL,
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * RefSpec:dbus_path:
   *
   * Path for this refspec on the bus
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DBUS_PATH,
                                   g_param_spec_string ("dbus-path",
                                                        NULL,
                                                        NULL,
                                                        NULL,
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_READABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

}


/**
 * refspec_is_updating:
 *
 * Returns: True if the instance is updating.
 */
gboolean
refspec_is_updating (RefSpec *self)
{

  return g_atomic_int_get (&self->updating) == TRUE;
}

/**
 * refspec_new:
 * @id: The refspec id
 *
 * Creates a new #RefSpec instance.
 *
 * Returns: A new #RPMOSTreeRefSpec. Free with g_object_unref().
 */
RPMOSTreeRefSpec *
refspec_new (const gchar *id)
{
  gs_free gchar *dbus_path = NULL;

  g_return_val_if_fail (id != NULL, NULL);

  dbus_path = utils_generate_object_path (BASE_DBUS_PATH,
                                          REFSPEC_DBUS_PATH_NAME,
                                          id,
                                          NULL);
  g_return_val_if_fail (dbus_path != NULL, NULL);

  return RPMOSTREE_REF_SPEC (g_object_new (RPM_OSTREE_TYPE_DAEMON_REFSPEC,
                                           "id", id,
                                           "dbus_path", dbus_path,
                                           NULL));
}


static void
refspec_iface_init (RPMOSTreeRefSpecIface *iface)
{
  iface->handle_get_rpm_diff = handle_get_rpm_diff;
  iface->handle_pull_rpm_db = handle_pull_rpm_db;
  iface->handle_deploy = handle_deploy;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * refspec_parse_partial:
 * @new_provided_refspec: The provided refspec
 * @current_refspec: The current refspec object that is in use.
 * @out_ref: Pointer to the new ref
 * @out_remote: Pointer to the new remote
 * @error: Pointer to an error pointer.
 *
 * Takes a refspec string and adds any missing bits based on the
 * current_refspec argument. Errors if a full valid refspec can't
 * be derived.
 *
 * Returns: True on sucess.
 */
static gboolean
refspec_parse_partial (const gchar *new_provided_refspec,
                       RefSpec *current_refspec,
                       gchar **out_ref,
                       gchar **out_remote,
                       GError **error)
{

  gs_free gchar *ref = NULL;
  gs_free gchar *remote = NULL;
  gs_free gchar *origin_ref = NULL;
  gs_free gchar *origin_remote = NULL;
  GError *parse_error = NULL;

  gboolean ret = FALSE;

  /* Allow just switching remotes */
  if (g_str_has_suffix (new_provided_refspec, ":"))
  {
    remote = g_strdup (new_provided_refspec);
    remote[strlen (remote) - 1] = '\0';
  }
  else
  {
    if (!ostree_parse_refspec (new_provided_refspec, &remote,
                               &ref, &parse_error))
      {
        g_set_error_literal (error, RPM_OSTREED_ERROR,
                     RPM_OSTREED_ERROR_INVALID_REFSPEC,
                     parse_error->message);
        g_clear_error (&parse_error);
        goto out;
      }
  }

  if (current_refspec != NULL)
    {
      origin_remote = rpmostree_ref_spec_dup_remote_name (RPMOSTREE_REF_SPEC(current_refspec ));
      origin_ref = rpmostree_ref_spec_dup_ref (RPMOSTREE_REF_SPEC(current_refspec ));
    }

  if (ref == NULL)
    {
      if (origin_ref)
        {
          ref = g_strdup (origin_ref);
        }

      else
        {
          g_set_error (error, RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_INVALID_REFSPEC,
                      "Could not determine default ref to pull.");
          goto out;
        }

    }
  else if (remote == NULL)
    {
      if (origin_remote)
        {
          remote = g_strdup (origin_remote);
        }
      else
        {
          g_set_error (error, RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_INVALID_REFSPEC,
                       "Could not determine default remote to pull.");
          goto out;
        }
    }

  if (g_strcmp0 (origin_remote, remote) == 0 &&
      g_strcmp0 (origin_ref, ref) == 0)
    {
      g_set_error (error, RPM_OSTREED_ERROR,
                   RPM_OSTREED_ERROR_INVALID_REFSPEC,
                   "Old and new refs are equal: %s:%s",
                   remote, ref);
      goto out;
    }

  *out_ref = g_strdup (ref);
  *out_remote = g_strdup (remote);
  ret = TRUE;
out:
  return ret;
}


static void
_default_callback (GObject *source_object,
                   GAsyncResult *res,
                   gpointer user_data)
{
    GTask *task = G_TASK (res);
    g_object_unref (task);
}

/**
 * refspec_resolve_partial_aysnc:
 * @new_provided_refspec: The provided refspec
 * @current_refspec: The current refspec object that is in use.
 * @callback: Used to check the the refspec is in fact valid.
 * @callback_data: user_data passed to the callback
 * @error: Pointer to an error pointer.
 *
 * Takes a refspec string and adds any missing bits based on the
 * current_refspec argument. Errors if a valid looking refspec can't
 * be derived. Otherwise starts a task to verify that the refspec
 * is valid.
 *
 * Returns: True if callback will be called..
 */
gboolean
refspec_resolve_partial_aysnc (gpointer source_object,
                               const gchar *new_provided_refspec,
                               RefSpec *current_refspec,
                               GAsyncReadyCallback callback,
                               gpointer callback_data,
                               GError **error)
{

  gs_free gchar *new_ref = NULL;
  gs_free gchar *new_remote = NULL;
  gboolean ret = FALSE;

  if (!callback)
    callback = _default_callback;

  if (refspec_parse_partial (new_provided_refspec,
                             current_refspec,
                             &new_ref,
                             &new_remote,
                             error))
    {
      GTask *task = NULL;
      task = daemon_get_new_task (daemon_get (),
                                  source_object,
                                  NULL,
                                  callback,
                                  callback_data);
      g_task_set_task_data (task,
                            g_strconcat (new_remote, ":", new_ref, NULL),
                            (GDestroyNotify) g_free);
      g_task_run_in_thread (task, _do_pull_nodata);
      ret = TRUE;
    }

  return ret;
}
