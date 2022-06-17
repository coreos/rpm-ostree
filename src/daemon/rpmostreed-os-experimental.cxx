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

#include <libglnx.h>

#include "rpmostree-origin.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-sysroot-core.h"
#include "rpmostree-util.h"
#include "rpmostreed-daemon.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-errors.h"
#include "rpmostreed-os-experimental.h"
#include "rpmostreed-sysroot.h"
#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-utils.h"

typedef struct _RpmostreedOSExperimentalClass RpmostreedOSExperimentalClass;

struct _RpmostreedOSExperimental
{
  RPMOSTreeOSSkeleton parent_instance;
};

struct _RpmostreedOSExperimentalClass
{
  RPMOSTreeOSSkeletonClass parent_class;
};

static void rpmostreed_osexperimental_iface_init (RPMOSTreeOSExperimentalIface *iface);

G_DEFINE_TYPE_WITH_CODE (RpmostreedOSExperimental, rpmostreed_osexperimental,
                         RPMOSTREE_TYPE_OSEXPERIMENTAL_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_OSEXPERIMENTAL,
                                                rpmostreed_osexperimental_iface_init));

/* ----------------------------------------------------------------------------------------------------
 */

static void
os_dispose (GObject *object)
{
  RpmostreedOSExperimental *self = RPMOSTREED_OSEXPERIMENTAL (object);
  const gchar *object_path;

  object_path = g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (self));
  if (object_path != NULL)
    {
      rpmostreed_daemon_unpublish (rpmostreed_daemon_get (), object_path, object);
    }

  G_OBJECT_CLASS (rpmostreed_osexperimental_parent_class)->dispose (object);
}

static void
os_constructed (GObject *object)
{
  G_GNUC_UNUSED RpmostreedOSExperimental *self = RPMOSTREED_OSEXPERIMENTAL (object);

  /* TODO Integrate with PolicyKit via the "g-authorize-method" signal. */

  G_OBJECT_CLASS (rpmostreed_osexperimental_parent_class)->constructed (object);
}

static void
rpmostreed_osexperimental_class_init (RpmostreedOSExperimentalClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = os_dispose;
  gobject_class->constructed = os_constructed;
}

static void
rpmostreed_osexperimental_init (RpmostreedOSExperimental *self)
{
}

static gboolean
osexperimental_handle_moo (RPMOSTreeOSExperimental *interface, GDBusMethodInvocation *invocation,
                           gboolean is_utf8)
{
  static const char ascii_cow[] = "\n"
                                  "                 (__)\n"
                                  "                 (oo)\n"
                                  "           /------\\/\n"
                                  "          / |    ||\n"
                                  "         *  /\\---/\\\n"
                                  "            ~~   ~~\n";
  const char *result = is_utf8 ? "🐄\n" : ascii_cow;
  rpmostree_osexperimental_complete_moo (interface, invocation, result);
  return TRUE;
}

static gboolean
prepare_live_fs_txn (RPMOSTreeOSExperimental *interface, GDBusMethodInvocation *invocation,
                     GVariant *arg_options, RpmostreedTransaction **out_txn, GError **error)
{
  glnx_unref_object RpmostreedTransaction *transaction = NULL;

  /* Try to merge with an existing transaction, otherwise start a new one. */
  RpmostreedSysroot *rsysroot = rpmostreed_sysroot_get ();

  if (!rpmostreed_sysroot_prep_for_txn (rsysroot, invocation, &transaction, error))
    return glnx_prefix_error_null (error, "Preparing sysroot for transaction");

  if (transaction == NULL)
    {
      g_autoptr (GCancellable) cancellable = g_cancellable_new ();
      glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
      if (!rpmostreed_sysroot_load_state (rpmostreed_sysroot_get (), cancellable, &ot_sysroot, NULL,
                                          error))
        return glnx_prefix_error (error, "Loading sysroot state");

      transaction = rpmostreed_transaction_new_apply_live (invocation, ot_sysroot, arg_options,
                                                           cancellable, error);
      if (transaction == NULL)
        return glnx_prefix_error (error, "Starting live fs transaction");
    }
  g_assert (transaction != NULL);

  rpmostreed_sysroot_set_txn (rsysroot, transaction);
  *out_txn = util::move_nullify (transaction);
  return TRUE;
}

static gboolean
osexperimental_handle_live_fs (RPMOSTreeOSExperimental *interface,
                               GDBusMethodInvocation *invocation, GVariant *arg_options)
{
  GError *local_error = NULL;
  glnx_unref_object RpmostreedTransaction *transaction = NULL;

  gboolean is_ok
      = prepare_live_fs_txn (interface, invocation, arg_options, &transaction, &local_error);
  if (!is_ok)
    {
      g_assert (local_error != NULL);
      g_dbus_method_invocation_take_error (invocation, local_error);
      return TRUE; /* 🔚 Early return */
    }

  g_assert (transaction != NULL);
  const char *client_address = rpmostreed_transaction_get_client_address (transaction);
  rpmostree_osexperimental_complete_live_fs (interface, invocation, client_address);

  return TRUE;
}

static gboolean
prepare_download_pkgs_txn (const gchar *const *queries, const char *source,
                           GUnixFDList **out_fd_list, GError **error)
{
  if (!queries || !*queries)
    return glnx_throw (error, "No queries passed");

  OstreeSysroot *sysroot = rpmostreed_sysroot_get_root (rpmostreed_sysroot_get ());
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  const char *osname = ostree_deployment_get_osname (booted_deployment);

  g_autoptr (OstreeDeployment) cfg_merge_deployment
      = ostree_sysroot_get_merge_deployment (sysroot, osname);
  g_autoptr (OstreeDeployment) origin_merge_deployment
      = rpmostree_syscore_get_origin_merge_deployment (sysroot, osname);

  /* but set the source root to be the origin merge deployment's so we pick up releasever */
  g_autofree char *origin_deployment_root
      = rpmostree_get_deployment_root (sysroot, origin_merge_deployment);

  g_autofree char *cfg_deployment_root
      = rpmostree_get_deployment_root (sysroot, cfg_merge_deployment);

  g_autoptr (GUnixFDList) fd_list = NULL;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  if (!rpmostree_find_and_download_packages (queries, source, origin_deployment_root,
                                             cfg_deployment_root, &fd_list, cancellable, error))
    return FALSE;

  *out_fd_list = util::move_nullify (fd_list);
  return TRUE;
}

static gboolean
osexperimental_handle_download_packages (RPMOSTreeOSExperimental *interface,
                                         GDBusMethodInvocation *invocation, GUnixFDList *_fds,
                                         const gchar *const *queries, const char *source)
{
  GError *local_error = NULL;
  GUnixFDList *fd_list = NULL;

  gboolean is_ok = prepare_download_pkgs_txn (queries, source, &fd_list, &local_error);
  if (!is_ok)
    {
      g_assert (local_error != NULL);
      g_dbus_method_invocation_take_error (invocation, local_error);
      return TRUE; /* 🔚 Early return */
    }

  g_assert (fd_list != NULL);
  rpmostree_osexperimental_complete_download_packages (interface, invocation, fd_list);

  return TRUE;
}

static void
rpmostreed_osexperimental_iface_init (RPMOSTreeOSExperimentalIface *iface)
{
  iface->handle_moo = osexperimental_handle_moo;
  iface->handle_live_fs = osexperimental_handle_live_fs;
  iface->handle_download_packages = osexperimental_handle_download_packages;
}

/* ----------------------------------------------------------------------------------------------------
 */

RPMOSTreeOSExperimental *
rpmostreed_osexperimental_new (OstreeSysroot *sysroot, OstreeRepo *repo, const char *name,
                               GError **error)
{
  g_assert (OSTREE_IS_SYSROOT (sysroot));
  g_assert (name != NULL);

  CXX_TRY_VAR (path,
               rpmostreecxx::generate_object_path (rust::Str (BASE_DBUS_PATH), rust::Str (name)),
               error);

  auto obj = (RpmostreedOSExperimental *)g_object_new (RPMOSTREED_TYPE_OSEXPERIMENTAL, NULL);

  rpmostreed_daemon_publish (rpmostreed_daemon_get (), path.c_str (), FALSE, obj);

  return RPMOSTREE_OSEXPERIMENTAL (obj);
}
