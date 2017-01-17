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
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmdb.h>
#include <libdnf/libdnf.h>

#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-sysroot.h"
#include "rpmostree-sysroot-upgrader.h"
#include "rpmostreed-utils.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-scripts.h"
#include "rpmostree-core.h"

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  char **packages_added;
  char **packages_removed;
  GHashTable *ignore_scripts;
  RpmOstreeTransactionPkgFlags flags;
} PkgChangeTransaction;

typedef RpmostreedTransactionClass PkgChangeTransactionClass;

GType pkg_change_transaction_get_type (void);

G_DEFINE_TYPE (PkgChangeTransaction,
               pkg_change_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
pkg_change_transaction_finalize (GObject *object)
{
  PkgChangeTransaction *self;

  self = (PkgChangeTransaction *) object;
  g_free (self->osname);
  g_strfreev (self->packages_added);
  g_strfreev (self->packages_removed);
  g_clear_pointer (&self->ignore_scripts, g_hash_table_unref);

  G_OBJECT_CLASS (pkg_change_transaction_parent_class)->finalize (object);
}

static gboolean
pkg_change_transaction_execute (RpmostreedTransaction *transaction,
			     GCancellable *cancellable,
			     GError **error)
{
  gboolean ret = FALSE;
  PkgChangeTransaction *self = NULL;
  OstreeSysroot *sysroot = NULL;
  glnx_unref_object RpmOstreeSysrootUpgrader *upgrader = NULL;
  int flags = 0;

  self = (PkgChangeTransaction *) transaction;
  sysroot = rpmostreed_transaction_get_sysroot (transaction);

  if (self->flags & RPMOSTREE_TRANSACTION_PKG_FLAG_DRY_RUN)
    flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN;
  if (self->flags & RPMOSTREE_TRANSACTION_PKG_FLAG_NOSCRIPTS)
    flags |= RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_NOSCRIPTS;

  upgrader = rpmostree_sysroot_upgrader_new (sysroot, self->osname, flags,
                                             cancellable, error);
  if (upgrader == NULL)
    {
      if (error && *error == NULL)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Could not create sysroot upgrader");
      goto out;
    }

  if (self->ignore_scripts)
    rpmostree_sysroot_upgrader_set_ignore_scripts (upgrader, self->ignore_scripts);

  if (self->packages_removed)
    {
      if (!rpmostree_sysroot_upgrader_delete_packages (upgrader, self->packages_removed,
						       cancellable, error))
	goto out;
    }

  if (self->packages_added)
    {
      if (!rpmostree_sysroot_upgrader_add_packages (upgrader, self->packages_added,
						    cancellable, error))
	goto out;
    }

  if (!rpmostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
    goto out;

  if (self->flags & RPMOSTREE_TRANSACTION_PKG_FLAG_REBOOT)
    rpmostreed_reboot (cancellable, error);

  ret = TRUE;
out:
  return ret;
}

static void
pkg_change_transaction_class_init (PkgChangeTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = pkg_change_transaction_finalize;

  class->execute = pkg_change_transaction_execute;
}

static void
pkg_change_transaction_init (PkgChangeTransaction *self)
{
}

static char **
strdupv_canonicalize (const char *const *strv)
{
  if (strv && *strv)
    return g_strdupv ((char**)strv);
  return NULL;
}

RpmostreedTransaction *
rpmostreed_transaction_new_pkg_change (GDBusMethodInvocation *invocation,
				       OstreeSysroot         *sysroot,
				       const char            *osname,
				       const char * const    *packages_added,
				       const char * const    *packages_removed,
				       const char * const    *ignore_scripts,
				       RpmOstreeTransactionPkgFlags flags,
				       GCancellable          *cancellable,
				       GError               **error)
{
  glnx_unref_object PkgChangeTransaction *self = NULL;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);
  g_return_val_if_fail (packages_added != NULL || packages_removed != NULL, NULL);

  self = g_initable_new (pkg_change_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->packages_added = strdupv_canonicalize (packages_added);
      self->packages_removed = strdupv_canonicalize (packages_removed);
      if (!rpmostree_script_ignore_hash_from_strv (ignore_scripts, &self->ignore_scripts, error))
	return NULL;
      self->flags = flags;
    }

  return (RpmostreedTransaction *) g_steal_pointer (&self);
}
