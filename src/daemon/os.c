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

#include "libglnx.h"

#include "types.h"
#include "os.h"
#include "utils.h"

typedef struct _OSStubClass OSStubClass;

struct _OSStub
{
  RPMOSTreeOSSkeleton parent_instance;

  GWeakRef sysroot;
};

struct _OSStubClass
{
  RPMOSTreeOSSkeletonClass parent_class;
};

enum {
  PROP_0,
  PROP_SYSROOT
};

static void osstub_iface_init (RPMOSTreeOSIface *iface);


G_DEFINE_TYPE_WITH_CODE (OSStub, osstub, RPMOSTREE_TYPE_OS_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_OS,
                                                osstub_iface_init));

static void
osstub_set_sysroot (OSStub *self,
                    OstreeSysroot *sysroot)
{
  g_weak_ref_set (&self->sysroot, sysroot);
}

static void
osstub_set_property (GObject *object,
                     guint property_id,
                     const GValue *value,
                     GParamSpec *pspec)
{
  OSStub *self = OSSTUB (object);

  switch (property_id)
    {
      case PROP_SYSROOT:
        osstub_set_sysroot (self, g_value_get_object (value));
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
osstub_get_property (GObject *object,
                     guint property_id,
                     GValue *value,
                     GParamSpec *pspec)
{
  OSStub *self = OSSTUB (object);

  switch (property_id)
    {
      case PROP_SYSROOT:
        g_value_take_object (value, osstub_ref_sysroot (self));
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
osstub_dispose (GObject *object)
{
  OSStub *self = OSSTUB (object);

  g_weak_ref_clear (&self->sysroot);

  G_OBJECT_CLASS (osstub_parent_class)->dispose (object);
}

static void
osstub_init (OSStub *self)
{
  g_weak_ref_init (&self->sysroot, NULL);
}

static void
osstub_class_init (OSStubClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->set_property = osstub_set_property;
  gobject_class->get_property = osstub_get_property;
  gobject_class->dispose = osstub_dispose;

  g_object_class_install_property (gobject_class,
                                   PROP_SYSROOT,
                                   g_param_spec_object ("sysroot",
                                                        NULL,
                                                        NULL,
                                                        OSTREE_TYPE_SYSROOT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
osstub_handle_get_cached_update_rpm_diff (RPMOSTreeOS *interface,
                                          GDBusMethodInvocation *invocation,
                                          const char *arg_deployid)
{
  /* FIXME */

  return TRUE;
}

static gboolean
osstub_handle_download_update_rpm_diff (RPMOSTreeOS *interface,
                                        GDBusMethodInvocation *invocation,
                                        const char *arg_deployid)
{
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  /* FIXME Do locking here, make sure we have exclusive access. */

  transaction = new_transaction (invocation, cancellable, NULL, &local_error);

  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_download_update_rpm_diff (interface,
                                                      invocation,
                                                      object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
osstub_handle_upgrade (RPMOSTreeOS *interface,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_options)
{
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  /* FIXME Do locking here, make sure we have exclusive access. */

  transaction = new_transaction (invocation, cancellable, NULL, &local_error);

  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_upgrade (interface,
                                     invocation,
                                     object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
osstub_handle_rollback (RPMOSTreeOS *interface,
                        GDBusMethodInvocation *invocation)
{
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  /* FIXME Do locking here, make sure we have exclusive access. */

  transaction = new_transaction (invocation, cancellable, NULL, &local_error);

  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_rollback (interface,
                                      invocation,
                                      object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
osstub_handle_clear_rollback_target (RPMOSTreeOS *interface,
                                     GDBusMethodInvocation *invocation)
{
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  /* FIXME Do locking here, make sure we have exclusive access. */

  transaction = new_transaction (invocation, cancellable, NULL, &local_error);

  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_clear_rollback_target (interface,
                                                   invocation,
                                                   object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static gboolean
osstub_handle_rebase (RPMOSTreeOS *interface,
                      GDBusMethodInvocation *invocation,
                      const char *arg_refspec,
                      const char * const *arg_packages)
{
  /* FIXME */

  return TRUE;
}

static gboolean
osstub_handle_get_cached_rebase_rpm_diff (RPMOSTreeOS *interface,
                                          GDBusMethodInvocation *invocation,
                                          const char *arg_refspec,
                                          const char * const *arg_packages)
{
  /* FIXME */

  return TRUE;
}

static gboolean
osstub_handle_download_rebase_rpm_diff (RPMOSTreeOS *interface,
                                        GDBusMethodInvocation *invocation,
                                        const char *arg_refspec,
                                        const char * const *arg_packages)
{
  glnx_unref_object RPMOSTreeTransaction *transaction = NULL;
  glnx_unref_object GCancellable *cancellable = NULL;
  GError *local_error = NULL;

  cancellable = g_cancellable_new ();

  /* FIXME Do locking here, make sure we have exclusive access. */

  transaction = new_transaction (invocation, cancellable, NULL, &local_error);

  if (local_error == NULL)
    {
      const char *object_path;

      object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (transaction));

      rpmostree_os_complete_download_rebase_rpm_diff (interface,
                                                      invocation,
                                                      object_path);
    }
  else
    {
      g_dbus_method_invocation_take_error (invocation, local_error);
    }

  return TRUE;
}

static void
osstub_iface_init (RPMOSTreeOSIface *iface)
{
  iface->handle_get_cached_update_rpm_diff = osstub_handle_get_cached_update_rpm_diff;
  iface->handle_download_update_rpm_diff   = osstub_handle_download_update_rpm_diff;
  iface->handle_upgrade                    = osstub_handle_upgrade;
  iface->handle_rollback                   = osstub_handle_rollback;
  iface->handle_clear_rollback_target      = osstub_handle_clear_rollback_target;
  iface->handle_rebase                     = osstub_handle_rebase;
  iface->handle_get_cached_rebase_rpm_diff = osstub_handle_get_cached_rebase_rpm_diff;
  iface->handle_download_rebase_rpm_diff   = osstub_handle_download_rebase_rpm_diff;
}

/* ---------------------------------------------------------------------------------------------------- */

RPMOSTreeOS *
osstub_new (OstreeSysroot *sysroot,
            const char *name)
{
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  return g_object_new (TYPE_OSSTUB, "sysroot", sysroot, "name", name, NULL);
}

OstreeSysroot *
osstub_ref_sysroot (OSStub *self)
{
  g_return_val_if_fail (IS_OSSTUB (self), NULL);

  return g_weak_ref_get (&self->sysroot);
}
