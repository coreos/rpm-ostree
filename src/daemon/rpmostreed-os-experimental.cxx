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
  GUnixFDList *fd_list = NULL;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();

  if (!queries || !*queries)
    return glnx_throw (error, "No queries passed");

  OstreeSysroot *sysroot = rpmostreed_sysroot_get_root (rpmostreed_sysroot_get ());
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  const char *osname = ostree_deployment_get_osname (booted_deployment);
  g_autoptr (GPtrArray) dnf_pkgs = g_ptr_array_new_with_free_func (g_object_unref);

  g_autoptr (OstreeDeployment) cfg_merge_deployment
      = ostree_sysroot_get_merge_deployment (sysroot, osname);
  g_autoptr (OstreeDeployment) origin_merge_deployment
      = rpmostree_syscore_get_origin_merge_deployment (sysroot, osname);

  /* but set the source root to be the origin merge deployment's so we pick up releasever */
  g_autofree char *origin_deployment_root
      = rpmostree_get_deployment_root (sysroot, origin_merge_deployment);

  OstreeRepo *repo = ostree_sysroot_repo (sysroot);
  g_autoptr (RpmOstreeContext) ctx = rpmostree_context_new_client (repo);
  rpmostree_context_set_dnf_caching (ctx, RPMOSTREE_CONTEXT_DNF_CACHE_FOREVER);
  /* We could bypass rpmostree_context_setup() here and call dnf_context_setup() ourselves
   * since we're not actually going to perform any installation. Though it does provide us
   * with the right semantics for install/source_root. */

  if (!rpmostree_context_setup (ctx, NULL, origin_deployment_root, cancellable, error))
    return glnx_prefix_error (error, "Setting up dnf context");

  /* point libdnf to our repos dir */
  rpmostree_context_configure_from_deployment (ctx, sysroot, cfg_merge_deployment);

  if (!rpmostree_context_download_metadata (ctx, DNF_CONTEXT_SETUP_SACK_FLAG_SKIP_RPMDB,
                                            cancellable, error))
    return glnx_prefix_error (error, "Downloading metadata");

  DnfSack *sack = dnf_context_get_sack (rpmostree_context_get_dnf (ctx));
  auto parsed_source = ROSCXX_TRY_VAL (parse_override_source (source), error);

  if (parsed_source.kind == rpmostreecxx::PackageOverrideSourceKind::Repo)
    {
      const char *source_name = parsed_source.name.c_str ();
      for (const char *const *it = queries; it && *it; it++)
        {
          auto pkg_name = static_cast<const char *> (*it);
          g_autoptr (GPtrArray) pkglist = NULL;
          HyNevra nevra = NULL;
          g_auto (HySubject) subject = hy_subject_create (pkg_name);
          hy_autoquery HyQuery query = hy_subject_get_best_solution (
              subject, sack, NULL, &nevra, FALSE, TRUE, TRUE, TRUE, FALSE);
          hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, source_name);
          pkglist = hy_query_run (query);
          if (!pkglist || pkglist->len == 0)
            return glnx_throw (error, "No matches for \"%s\" in repo '%s'", pkg_name, source_name);

          g_ptr_array_add (dnf_pkgs, g_object_ref (pkglist->pdata[0]));
        }
    }
  /* Future source kinds go here */
  else
    return glnx_throw (error, "Unsupported source type");

  rpmostree_set_repos_on_packages (rpmostree_context_get_dnf (ctx), dnf_pkgs);

  if (!rpmostree_download_packages (dnf_pkgs, cancellable, error))
    return glnx_prefix_error (error, "Downloading packages");

  fd_list = g_unix_fd_list_new ();
  for (unsigned int i = 0; i < dnf_pkgs->len; i++)
    {
      DnfPackage *pkg = static_cast<DnfPackage *> (dnf_pkgs->pdata[i]);
      const gchar *path = dnf_package_get_filename (pkg);
      gint fd = -1;

      if (!glnx_openat_rdonly (AT_FDCWD, path, TRUE, &fd, error))
        return FALSE;

      if (g_unix_fd_list_append (fd_list, fd, error) < 0)
        return FALSE;

      if (!glnx_unlinkat (AT_FDCWD, path, 0, error))
        return FALSE;
    }

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
rpmostreed_osexperimental_new (OstreeSysroot *sysroot, OstreeRepo *repo, const char *name)
{
  g_assert (OSTREE_IS_SYSROOT (sysroot));
  g_assert (name != NULL);

  g_autofree char *path = rpmostreed_generate_object_path (BASE_DBUS_PATH, name, NULL);

  auto obj = (RpmostreedOSExperimental *)g_object_new (RPMOSTREED_TYPE_OSEXPERIMENTAL, NULL);

  rpmostreed_daemon_publish (rpmostreed_daemon_get (), path, FALSE, obj);

  return RPMOSTREE_OSEXPERIMENTAL (obj);
}
