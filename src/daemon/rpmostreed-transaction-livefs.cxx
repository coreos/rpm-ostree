/*
 * Copyright (C) 2015,2017 Red Hat, Inc.
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

#include <sys/types.h>
#include <sys/xattr.h>
#include <systemd/sd-journal.h>
#include <libglnx.h>

#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-sysroot.h"
#include "rpmostree-sysroot-core.h"
#include "rpmostree-util.h"
#include "rpmostree-origin.h"
#include "rpmostree-db.h"
#include "rpmostree-output.h"
#include "rpmostree-core.h"
#include "rpmostree-cxxrs.h"
#include "rpmostreed-utils.h"

typedef struct {
  RpmostreedTransaction parent;
  GVariant *options;
} LiveFsTransaction;

typedef RpmostreedTransactionClass LiveFsTransactionClass;

GType livefs_transaction_get_type (void);

G_DEFINE_TYPE (LiveFsTransaction,
               livefs_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
livefs_transaction_finalize (GObject *object)
{
  G_GNUC_UNUSED LiveFsTransaction *self;

  self = (LiveFsTransaction *) object;
  g_variant_unref (self->options);

  G_OBJECT_CLASS (livefs_transaction_parent_class)->finalize (object);
}

static gboolean
livefs_transaction_execute (RpmostreedTransaction *transaction,
                            GCancellable *cancellable,
                            GError **error)
{
  LiveFsTransaction *self = (LiveFsTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);

  g_auto(GVariantDict) options_dict;
  g_variant_dict_init (&options_dict, self->options);
  const char *target = NULL;
  (void) g_variant_dict_lookup (&options_dict, "target", "&s", &target);

  /* Run the transaction */
  try {
    rpmostreecxx::transaction_livefs(*sysroot, target ?: "");
  } catch (std::exception& e) {
    (void) rpmostree_syscore_bump_mtime (sysroot, NULL);
    throw;
  }

  /* We use this to notify ourselves of changes, which is a bit silly, but it
   * keeps things consistent if `ostree admin` is invoked directly.  Always
   * invoke it, in case we error out, so that we correctly update for the
   * partial state.
   */
  return rpmostree_syscore_bump_mtime (sysroot, error);
}

static void
livefs_transaction_class_init (LiveFsTransactionClass *clazz)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (clazz);
  object_class->finalize = livefs_transaction_finalize;

  clazz->execute = livefs_transaction_execute;
}

static void
livefs_transaction_init (LiveFsTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_livefs (GDBusMethodInvocation *invocation,
                                   OstreeSysroot         *sysroot,
                                   GVariant              *options,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);

  auto self = (LiveFsTransaction *)g_initable_new (livefs_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    self->options = g_variant_ref (options);

  return (RpmostreedTransaction *) self;
}
