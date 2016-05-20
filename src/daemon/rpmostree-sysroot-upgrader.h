/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <ostree.h>

G_BEGIN_DECLS

#define RPMOSTREE_TYPE_SYSROOT_UPGRADER rpmostree_sysroot_upgrader_get_type()
#define RPMOSTREE_SYSROOT_UPGRADER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), RPMOSTREE_TYPE_SYSROOT_UPGRADER, RpmOstreeSysrootUpgrader))
#define RPMOSTREE_IS_SYSROOT_UPGRADER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), RPMOSTREE_TYPE_SYSROOT_UPGRADER))

typedef struct RpmOstreeSysrootUpgrader RpmOstreeSysrootUpgrader;

/**
 * RpmOstreeSysrootUpgraderFlags:
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_NONE: No options
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED: Do not error if the origin has an unconfigured-state key
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER: Do not error if the new deployment was composed earlier than the current deployment
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_REDEPLOY: Use the same revision as the current deployment
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN: If layering packages, only print the transaction
 *
 * Flags controlling operation of an #RpmOstreeSysrootUpgrader.
 */
typedef enum {
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_NONE                 = (1 << 0),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED  = (1 << 1),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER          = (1 << 2),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_REDEPLOY             = (1 << 3),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_DRY_RUN   = (1 << 4)
} RpmOstreeSysrootUpgraderFlags;

GType rpmostree_sysroot_upgrader_get_type (void);

GType rpmostree_sysroot_upgrader_flags_get_type (void);

RpmOstreeSysrootUpgrader *rpmostree_sysroot_upgrader_new (OstreeSysroot              *sysroot,
                                                          const char                 *osname,
                                                          RpmOstreeSysrootUpgraderFlags  flags,
                                                          GCancellable               *cancellable,
                                                          GError                    **error);

OstreeDeployment* rpmostree_sysroot_upgrader_get_merge_deployment (RpmOstreeSysrootUpgrader *self);
const char *rpmostree_sysroot_upgrader_get_refspec (RpmOstreeSysrootUpgrader *self);
const char *const*rpmostree_sysroot_upgrader_get_packages (RpmOstreeSysrootUpgrader *self);

char * rpmostree_sysroot_upgrader_get_origin_description (RpmOstreeSysrootUpgrader *self);

GKeyFile *rpmostree_sysroot_upgrader_get_origin (RpmOstreeSysrootUpgrader *self);
GKeyFile *rpmostree_sysroot_upgrader_dup_origin (RpmOstreeSysrootUpgrader *self);
gboolean rpmostree_sysroot_upgrader_set_origin (RpmOstreeSysrootUpgrader *self, GKeyFile *origin,
                                             GCancellable *cancellable, GError **error);

gboolean rpmostree_sysroot_upgrader_set_origin_rebase (RpmOstreeSysrootUpgrader *self,
                                                       const char *new_refspec,
                                                       GError **error);

void rpmostree_sysroot_upgrader_set_origin_override (RpmOstreeSysrootUpgrader *self,
                                                     const char *override_commit);

gboolean
rpmostree_sysroot_upgrader_add_packages (RpmOstreeSysrootUpgrader *self,
                                         char                    **new_packages,
                                         GCancellable             *cancellable,
                                         GError                  **error);

gboolean
rpmostree_sysroot_upgrader_delete_packages (RpmOstreeSysrootUpgrader *self,
                                            char                    **packages,
                                            GCancellable             *cancellable,
                                            GError                  **error);

gboolean
rpmostree_sysroot_upgrader_pull (RpmOstreeSysrootUpgrader  *self,
                                 const char             *dir_to_pull,
                                 OstreeRepoPullFlags     flags,
                                 OstreeAsyncProgress    *progress,
                                 gboolean               *out_changed,
                                 GCancellable           *cancellable,
                                 GError                **error);

gboolean
rpmostree_sysroot_upgrader_deploy (RpmOstreeSysrootUpgrader  *self,
                                   GCancellable           *cancellable,
                                   GError                **error);

G_END_DECLS
