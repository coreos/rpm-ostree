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
#include <libhif/libhif.h>
#include <libgsystem.h>

#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-sysroot.h"
#include "rpmostree-sysroot-upgrader.h"
#include "rpmostreed-utils.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-core.h"

typedef struct {
  RpmostreedTransaction parent;
  char *osname;
  char **packages;
  RpmOstreeTransactionPkgFlags flags;
} PkgAddTransaction;

typedef RpmostreedTransactionClass PkgAddTransactionClass;

GType pkg_add_transaction_get_type (void);

G_DEFINE_TYPE (PkgAddTransaction,
               pkg_add_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
pkg_add_transaction_finalize (GObject *object)
{
  PkgAddTransaction *self;

  self = (PkgAddTransaction *) object;
  g_free (self->osname);
  g_strfreev (self->packages);

  G_OBJECT_CLASS (pkg_add_transaction_parent_class)->finalize (object);
}

static gboolean
pkg_add_transaction_execute (RpmostreedTransaction *transaction,
			     GCancellable *cancellable,
			     GError **error)
{
  gboolean ret = FALSE;
  PkgAddTransaction *self = NULL;
  OstreeSysroot *sysroot = NULL;
  glnx_unref_object RpmOstreeSysrootUpgrader *upgrader = NULL;
  int flags = RPMOSTREE_SYSROOT_UPGRADER_FLAGS_REDEPLOY;

  self = (PkgAddTransaction *) transaction;
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

  if (!rpmostree_sysroot_upgrader_add_packages (upgrader, self->packages,
                                                cancellable, error))
    goto out;

  if (!rpmostree_sysroot_upgrader_deploy (upgrader, cancellable, error))
    goto out;

  if (self->flags & RPMOSTREE_TRANSACTION_PKG_FLAG_REBOOT)
    rpmostreed_reboot (cancellable, error);

  ret = TRUE;
out:
  return ret;
}

static void
pkg_add_transaction_class_init (PkgAddTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = pkg_add_transaction_finalize;

  class->execute = pkg_add_transaction_execute;
}

static void
pkg_add_transaction_init (PkgAddTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_pkg_add (GDBusMethodInvocation *invocation,
				    OstreeSysroot         *sysroot,
				    const char            *osname,
				    const char * const    *packages,
				    RpmOstreeTransactionPkgFlags flags,
				    GCancellable          *cancellable,
				    GError               **error)
{
  PkgAddTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);
  g_return_val_if_fail (osname != NULL, NULL);
  g_return_val_if_fail (packages != NULL, NULL);

  self = g_initable_new (pkg_add_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->osname = g_strdup (osname);
      self->packages = g_strdupv ((char**)packages);
      self->flags = flags;
    }

  return (RpmostreedTransaction *) self;
}
