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

#include "daemon.h"
#include "manager.h"
#include "sysroot.h"
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

  GHashTable *sysroots;

  GCancellable *cancellable;
};

struct _ManagerClass
{
  RPMOSTreeManagerSkeletonClass parent_class;
};

static void manager_iface_init (RPMOSTreeManagerIface *iface);

G_DEFINE_TYPE_WITH_CODE (Manager, manager, RPMOSTREE_TYPE_MANAGER_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_MANAGER,
                                                manager_iface_init));

static Manager *_manager_instance;

/* ---------------------------------------------------------------------------------------------------- */

static void
manager_dispose (GObject *object)
{
  Manager *self = MANAGER (object);
  GHashTableIter diter;
  gpointer value;

  g_cancellable_cancel (self->cancellable);

  g_hash_table_iter_init (&diter, self->sysroots);
  while (g_hash_table_iter_next (&diter, NULL, &value))
    g_object_run_dispose (value);
  g_hash_table_remove_all (self->sysroots);

  G_OBJECT_CLASS (manager_parent_class)->dispose (object);
}

static void
manager_finalize (GObject *object)
{
  Manager *self = MANAGER (object);
  _manager_instance = NULL;

  g_clear_object (&self->cancellable);
  g_hash_table_unref (self->sysroots);

  G_OBJECT_CLASS (manager_parent_class)->finalize (object);
}

static void
manager_init (Manager *manager)
{
  g_assert (_manager_instance == NULL);
  _manager_instance = manager;

  manager->sysroots = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify) g_object_unref);
}


static Sysroot *
_manager_get_sysroot (Manager *self,
                      const gchar *path,
                      const gchar *name,
                      GError **error)
{
  Sysroot *sysroot = NULL;
  sysroot = g_hash_table_lookup (self->sysroots, path);
  if (sysroot == NULL)
    {
      if (sysroot_publish_new (path, name, &sysroot, error))
        {
          g_hash_table_insert (self->sysroots,
                               g_strdup (path),
                               g_object_ref (sysroot));
          g_object_unref (sysroot);
        }
    }

  return sysroot;
}


static gboolean
handle_get_sysroot (RPMOSTreeManager *object,
                    GDBusMethodInvocation *invocation,
                    const gchar *path)
{
  GError *error = NULL;
  GError *resp_error = NULL;
  gs_free gchar *dbus_path = NULL;
  Sysroot *sysroot = NULL;

  Manager *self = MANAGER (object);

  g_debug ("Getting sysroot %s", path);

  sysroot = _manager_get_sysroot (self, path, path, &error);
  if (sysroot != NULL)
    {
      g_object_get (sysroot, "dbus-path", &dbus_path, NULL);
      sysroot_watch_client_if_needed (sysroot,
                                      g_dbus_method_invocation_get_connection (invocation),
                                      g_dbus_method_invocation_get_sender (invocation));

      rpmostree_manager_complete_get_sysroot (object, invocation, dbus_path);
    }
  else
    {
      g_set_error (&resp_error,
                   RPM_OSTREED_ERROR,
                   RPM_OSTREED_ERROR_INVALID_SYSROOT,
                   error->message, NULL);
      g_clear_error (&error);
      g_dbus_method_invocation_take_error (invocation, resp_error);
    }
  return TRUE;
}

static void
manager_constructed (GObject *object)
{
  Manager *manager = MANAGER (object);
  GError *error = NULL;
  Sysroot *sysroot = NULL;
  gs_free gchar *dbus_path = NULL;

  g_debug ("constructing manager");
  sysroot = _manager_get_sysroot (manager, SYSROOT_DEFAULT_PATH,
                                  "default", &error);
  if (sysroot != NULL)
    {
      g_object_get (sysroot, "dbus-path", &dbus_path, NULL);
      rpmostree_manager_set_default_sysroot (RPMOSTREE_MANAGER (manager),
                                             g_strdup (dbus_path));
    }
  else
    {
      g_message ("Couldn't load default sysroot: %s", error->message);
    }

  g_clear_error (&error);

  g_signal_connect (RPMOSTREE_MANAGER(manager), "g-authorize-method",
                    G_CALLBACK (auth_check_root_or_access_denied), NULL);

  G_OBJECT_CLASS (manager_parent_class)->constructed (object);
}

static void
manager_class_init (ManagerClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = manager_dispose;
  gobject_class->finalize     = manager_finalize;
  gobject_class->constructed  = manager_constructed;
}


static void
manager_iface_init (RPMOSTreeManagerIface *iface)
{
  iface->handle_get_sysroot = handle_get_sysroot;
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
