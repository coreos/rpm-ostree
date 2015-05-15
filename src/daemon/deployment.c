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

#include "deployment.h"
#include "refspec.h"
#include "daemon.h"
#include "manager.h"
#include "auth.h"
#include "utils.h"

#include "libgsystem.h"
#include "libglnx.h"

/**
 * SECTION:daemon
 * @title: Deployment
 * @short_description: Implementation of #RPMOSTreeDeployment
 *
 * This type provides an implementation of the #RPMOSTreeDeployment interface.
 */

typedef struct _DeploymentClass DeploymentClass;
struct _Deployment
{
  RPMOSTreeDeploymentSkeleton parent_instance;

  gchar *id;
  gchar *dbus_path;
  gchar *rel_path;

  GCancellable *cancellable;
};

struct _DeploymentClass
{
  RPMOSTreeDeploymentSkeletonClass parent_class;
};

enum
{
  PROP_0,
  PROP_DBUS_PATH,
  PROP_ID
};

static void deployment_iface_init (RPMOSTreeDeploymentIface *iface);

G_DEFINE_TYPE_WITH_CODE (Deployment, deployment, RPMOSTREE_TYPE_DEPLOYMENT_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_DEPLOYMENT,
                                                deployment_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_get_rpm_diff (RPMOSTreeDeployment *object,
                     GDBusMethodInvocation *invocation)
{
  Deployment *self = DEPLOYMENT (object);
  gchar *csum = rpmostree_deployment_dup_checksum (object); //freed by task

  GTask *task = daemon_get_new_task (daemon_get (),
                                     self,
                                     self->cancellable,
                                     utils_task_result_invoke,
                                     invocation);
  g_task_set_task_data (task, csum, g_free);
  g_task_run_in_thread (task, utils_get_diff_variant_in_thread);

  return TRUE;
}

static void
_do_make_default_thread (GTask         *task,
                         gpointer       object,
                         gpointer       data_ptr,
                         GCancellable  *cancellable)
{
  Deployment *self = DEPLOYMENT (object);

  gs_unref_object OstreeSysroot *ot_sysroot = NULL;
  gs_unref_object OstreeRepo *ot_repo = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_unref_ptrarray GPtrArray *new_deployments = NULL;
  GError *error = NULL;

  guint i;
  guint spot = 0;

  g_debug ("Starting threaded");
  if (!utils_load_sysroot_and_repo (manager_get_sysroot_path ( manager_get ()),
                                    cancellable,
                                    &ot_sysroot,
                                    &ot_repo,
                                    &error))
    goto out;

  g_debug ("loading deployment");
  deployments = ostree_sysroot_get_deployments (ot_sysroot);
  new_deployments = g_ptr_array_new_with_free_func (g_object_unref);

  if (deployments)
    {
      // g_ptr_array_insert would be so much better
      // but only >2.40

      // find the spot
      for (i = 0; i < deployments->len; i++)
      {
        gs_free gchar *id = deployment_generate_id (deployments->pdata[i]);
        gint c = g_strcmp0 (id, self->id);
        if (c == 0)
          {
            spot = i;
            break;
          }
      }

      // build out the reordered array
      g_ptr_array_add (new_deployments, g_object_ref (deployments->pdata[spot]));
      for (i = 0; i < deployments->len; i++)
      {
        if (i == spot)
          continue;
        g_ptr_array_add (new_deployments, g_object_ref (deployments->pdata[i]));
      }

      // if default changed write it.
      if (deployments->pdata[0] != new_deployments->pdata[0])
        ostree_sysroot_write_deployments (ot_sysroot,
                                          new_deployments,
                                          cancellable,
                                          &error);
    }

out:
  if (error == NULL)
      g_task_return_boolean (task, TRUE);
  else
      g_task_return_error (task, error);
}


static void
_task_callback (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  GError *error = NULL;
  gs_unref_object GTask *task = G_TASK (res);
  gs_free gchar *message = NULL;

  gboolean success = TRUE;

  success = g_task_propagate_boolean (task, &error);
  if (!success)
      message = g_strdup (error->message);
  else
      message = g_strdup ("Sucessfully reset deployment order");

  manager_end_update_operation (manager_get (),
                                success,
                                message,
                                success);

  g_clear_error (&error);
}

static gboolean
handle_make_default (RPMOSTreeDeployment *object,
                     GDBusMethodInvocation *invocation)
{
  Deployment *self = DEPLOYMENT (object);
  GTask *task = NULL;

  if (manager_begin_update_operation (manager_get (),
                                      invocation,
                                      "rebase"))
    {
      rpmostree_deployment_complete_make_default (object, invocation);
      task = daemon_get_new_task (daemon_get (),
                                  self,
                                  self->cancellable,
                                  _task_callback,
                                  self);
      g_task_run_in_thread (task, _do_make_default_thread);
    }

  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * deployment_generate_id :
 * @ostree_deployment: A #OstreeDeployment
 *
 * Returns: An id to identify this deployment..
 */
gchar *
deployment_generate_id (OstreeDeployment *ostree_deployment)
{
  const gchar *osname = ostree_deployment_get_osname (ostree_deployment);
  const guint hash = ostree_deployment_hash (ostree_deployment);
  return g_strdup_printf ("%s_%d", osname, hash);
}


static gboolean
deployment_add_gpg_results (Deployment *self,
                            OstreeRepo *repo,
                            const gchar *origin_refspec,
                            const gchar *csum,
                            GVariantBuilder *builder)
{
  GError *error = NULL;
  gboolean ret = FALSE;
  gs_free gchar *remote = NULL;
  gs_unref_object OstreeGpgVerifyResult *result = NULL;
  guint n_sigs, i;
  gboolean gpg_verify;

  if (!ostree_parse_refspec (origin_refspec, &remote, NULL, &error))
    goto out;


  if (!ostree_repo_remote_get_gpg_verify (repo, remote, &gpg_verify, &error))
    goto out;

  if (!gpg_verify)
    goto out;


  result = ostree_repo_verify_commit_ext (repo, csum, NULL, NULL,
                                          self->cancellable, &error);
  if (!result)
      goto out;

  n_sigs = ostree_gpg_verify_result_count_all (result);

  for (i = 0; i < n_sigs; i++)
    {
      g_variant_builder_add (builder, "v",
                             ostree_gpg_verify_result_get_all (result, i));
    }
  ret = TRUE;

out:

  /* NOT_FOUND just means the commit is not signed. */
  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_warning ("error loading gpg verify result %s", error->message);

  g_clear_error (&error);
  return ret;
}


/**
 * deployment_populate :
 * @deployment: A #Deployment
 * @ostree_deployment: A #OstreeDeployment
 * @deployment: A #OstreeRepo
 * @publish: A gboolean, should we publish
 *
 * Populates dbus properties and optionally
 * publishes the interface.
 *
 * Returns: A gboolean indicating success
 */
gboolean
deployment_populate (Deployment *deployment,
                     OstreeDeployment *ostree_deployment,
                     OstreeRepo *repo,
                     gboolean publish)
{
  gs_unref_variant GVariant *commit = NULL;
  GVariantBuilder builder;

  GError *error = NULL;
  g_autoptr (GKeyFile) origin = NULL;

  gs_free gchar *origin_refspec = NULL;
  gs_free gchar *version_commit = NULL;
  gs_free gchar *refpath = NULL;

  gboolean ret = FALSE;
  guint64 timestamp = 0;

  gint index = ostree_deployment_get_index (ostree_deployment);
  const gchar *osname = ostree_deployment_get_osname (ostree_deployment);
  const gchar *csum = ostree_deployment_get_csum (ostree_deployment);
  gint serial = ostree_deployment_get_deployserial (ostree_deployment);

  if (ostree_repo_load_variant (repo,
                                OSTREE_OBJECT_TYPE_COMMIT,
                                csum,
                                &commit,
                                &error))
    {
      gs_unref_variant GVariant *metadata = NULL;
      timestamp = ostree_commit_get_timestamp (commit);
      metadata = g_variant_get_child_value (commit, 0);
      if (metadata != NULL)
          g_variant_lookup (metadata, "version", "s", &version_commit);
    }
  else
    {
      g_warning ("error loading commit %s", error->message);
    }


  deployment->rel_path = ostree_deployment_get_origin_relpath (ostree_deployment);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));
  origin = ostree_deployment_get_origin (ostree_deployment);
  if (origin)
    {
      origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
      if (origin_refspec)
        {

          refpath = utils_generate_object_path (BASE_DBUS_PATH,
                                                REFSPEC_DBUS_PATH_NAME,
                                                origin_refspec,
                                                NULL);
          deployment_add_gpg_results (deployment,
                                      repo,
                                      origin_refspec,
                                      csum,
                                      &builder);


        }
    }

  rpmostree_deployment_set_index (RPMOSTREE_DEPLOYMENT (deployment), index);
  rpmostree_deployment_set_serial (RPMOSTREE_DEPLOYMENT (deployment), serial);
  rpmostree_deployment_set_checksum (RPMOSTREE_DEPLOYMENT (deployment), csum);
  rpmostree_deployment_set_version (RPMOSTREE_DEPLOYMENT (deployment), version_commit);
  rpmostree_deployment_set_osname (RPMOSTREE_DEPLOYMENT (deployment),
                                   osname ? osname : "");
  rpmostree_deployment_set_timestamp (RPMOSTREE_DEPLOYMENT (deployment),
                                      timestamp ? timestamp : 0);
  rpmostree_deployment_set_origin_refspec (RPMOSTREE_DEPLOYMENT (deployment),
                                           origin_refspec ? origin_refspec : "");
  rpmostree_deployment_set_refspec_objectpath (RPMOSTREE_DEPLOYMENT (deployment),
                                               refpath ? refpath : "");
  rpmostree_deployment_set_signatures (RPMOSTREE_DEPLOYMENT (deployment),
                                       g_variant_builder_end (&builder));

  ret = TRUE;
  if (publish)
    {
      daemon_publish (daemon_get (), deployment->dbus_path, FALSE, deployment);
      g_debug ("deployment %s published", deployment->id);
    }
  else
    {
      g_debug ("deployment %s updated", deployment->id);
    }

  g_clear_error (&error);
  return ret;
}


gint
deployment_index_compare (gconstpointer a,
                          gconstpointer b)
{
  Deployment *d1 = *(Deployment**)a;
  Deployment *d2 = *(Deployment**)b;

  return rpmostree_deployment_get_index (RPMOSTREE_DEPLOYMENT (d1)) -
         rpmostree_deployment_get_index (RPMOSTREE_DEPLOYMENT (d2));
}

static void
cancel_tasks (Deployment *self)
{
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();
}


/**
 * deployment_get_refspec :
 * @self: A #Deployment
 *
 * Returns: A RefSpec instance that is the current
 * origin refspec for this deployment. Takes a ref
 * so free with g_object_unref ().
 */
RefSpec *
deployment_get_refspec (Deployment *self)
{
  RefSpec *refspec = NULL;
  gs_free gchar *default_path = rpmostree_deployment_dup_refspec_objectpath (RPMOSTREE_DEPLOYMENT (self));
  if (default_path != NULL && g_strcmp0 (default_path, "/") != 0)
    {
      refspec = REFSPEC (daemon_get_interface (daemon_get (),
                                               default_path,
                                               "org.projectatomic.rpmostree1.RefSpec"));
    }

  return refspec;
}

static void
deployment_dispose (GObject *object)
{
  Deployment *self = DEPLOYMENT (object);

  g_cancellable_cancel (self->cancellable);
  daemon_unpublish (daemon_get (), self->dbus_path, self);
  g_signal_handlers_disconnect_by_data (manager_get (), self);

  G_OBJECT_CLASS (deployment_parent_class)->dispose (object);
}

static void
deployment_finalize (GObject *object)
{
  Deployment *deployment = DEPLOYMENT (object);
  g_free (deployment->id);
  g_free (deployment->dbus_path);
  g_free (deployment->rel_path);
  g_clear_object (&deployment->cancellable);
  G_OBJECT_CLASS (deployment_parent_class)->finalize (object);
}


static void
deployment_get_property (GObject *object,
                         guint prop_id,
                         GValue *value,
                         GParamSpec *pspec)
{
  Deployment *self = DEPLOYMENT (object);

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
deployment_set_property (GObject *object,
                      guint prop_id,
                      const GValue *value,
                      GParamSpec *pspec)
{
  Deployment *deployment = DEPLOYMENT (object);

  switch (prop_id)
    {
    case PROP_ID:
      g_assert (deployment->id == NULL);
      deployment->id = g_value_dup_string (value);
      break;
    case PROP_DBUS_PATH:
      g_assert (deployment->dbus_path == NULL);
      deployment->dbus_path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
deployment_init (Deployment *deployment)
{
  deployment->cancellable = g_cancellable_new ();
  deployment->rel_path = NULL;
}

static void
deployment_constructed (GObject *object)
{
  Deployment *self = DEPLOYMENT (object);

  G_OBJECT_CLASS (deployment_parent_class)->constructed (object);

  g_signal_connect (RPMOSTREE_DEPLOYMENT(self), "g-authorize-method",
                    G_CALLBACK (auth_check_root_or_access_denied), NULL);
  g_signal_connect_swapped (manager_get (), "cancel-tasks",
                            G_CALLBACK (cancel_tasks), self);
}

static void
deployment_class_init (DeploymentClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose      = deployment_dispose;
  gobject_class->finalize     = deployment_finalize;
  gobject_class->constructed  = deployment_constructed;
  gobject_class->set_property = deployment_set_property;
  gobject_class->get_property = deployment_get_property;

  /**
   * Deployment:id:
   *
   * The Deployment id
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
   * Deployment:dbus_path:
   *
   * Path for this deployment on the bus
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
 * deployment_new:
 * @id: The deployment id
 *
 * Creates a new #Deployment instance.
 *
 * Returns: A new #RPMOSTreeDeployment. Free with g_object_unref().
 */
RPMOSTreeDeployment *
deployment_new (gchar *id)
{
  gs_free gchar *dbus_path = NULL;
  g_return_val_if_fail (id != NULL, NULL);

  dbus_path = utils_generate_object_path (BASE_DBUS_PATH,
                                          DEPLOYMENT_DBUS_PATH_NAME,
                                          id,
                                          NULL);

  g_return_val_if_fail (dbus_path != NULL, NULL);

  return RPMOSTREE_DEPLOYMENT (g_object_new (RPM_OSTREE_TYPE_DAEMON_DEPLOYMENT,
                                             "id", id,
                                             "dbus_path", dbus_path,
                                             NULL));
}

static void
deployment_iface_init (RPMOSTreeDeploymentIface *iface)
{
  iface->handle_get_rpm_diff = handle_get_rpm_diff;
  iface->handle_make_default = handle_make_default;
}
