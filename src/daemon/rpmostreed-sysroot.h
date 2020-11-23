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

#pragma once

#include "ostree.h"
#include "rpmostreed-types.h"
#include <polkit/polkit.h>

G_BEGIN_DECLS

#define RPMOSTREED_TYPE_SYSROOT   (rpmostreed_sysroot_get_type ())
#define RPMOSTREED_SYSROOT(o)     (G_TYPE_CHECK_INSTANCE_CAST ((o), RPMOSTREED_TYPE_SYSROOT, RpmostreedSysroot))
#define RPMOSTREED_IS_SYSROOT(o)  (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPMOSTREED_TYPE_SYSROOT))

#define SYSROOT_DEFAULT_PATH "/"

GType               rpmostreed_sysroot_get_type         (void) G_GNUC_CONST;

RpmostreedSysroot * rpmostreed_sysroot_get              (void);

gboolean            rpmostreed_sysroot_populate         (RpmostreedSysroot *self,
                                                         GCancellable *cancellable,
                                                         GError **error);
gboolean            rpmostreed_sysroot_reload           (RpmostreedSysroot *self,
                                                         GError **error);

OstreeSysroot *     rpmostreed_sysroot_get_root         (RpmostreedSysroot *self);
OstreeRepo *        rpmostreed_sysroot_get_repo         (RpmostreedSysroot *self);
PolkitAuthority *   rpmostreed_sysroot_get_polkit_authority (RpmostreedSysroot *self);
gboolean            rpmostreed_sysroot_is_on_session_bus    (RpmostreedSysroot *self);

gboolean            rpmostreed_sysroot_load_state       (RpmostreedSysroot *self,
                                                         GCancellable *cancellable,
                                                         OstreeSysroot **out_sysroot,
                                                         OstreeRepo **out_repo,
                                                         GError **error);

gboolean            rpmostreed_sysroot_prep_for_txn (RpmostreedSysroot     *self,
                                                     GDBusMethodInvocation *invocation,
                                                     RpmostreedTransaction **out_compat_txn,
                                                     GError               **error);

gboolean            rpmostreed_sysroot_has_txn (RpmostreedSysroot     *self);

void                rpmostreed_sysroot_finish_txn (RpmostreedSysroot     *self,
                                                   RpmostreedTransaction *txn);

void                rpmostreed_sysroot_set_txn (RpmostreedSysroot     *self,
                                                RpmostreedTransaction *txn);

void                rpmostreed_sysroot_emit_update      (RpmostreedSysroot *self);

G_END_DECLS
