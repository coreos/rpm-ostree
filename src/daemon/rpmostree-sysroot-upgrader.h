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
#include "rpmostree-origin.h"
#include "rpmostree-sysroot-core.h"

G_BEGIN_DECLS

#define RPMOSTREE_TYPE_SYSROOT_UPGRADER rpmostree_sysroot_upgrader_get_type()
#define RPMOSTREE_SYSROOT_UPGRADER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), RPMOSTREE_TYPE_SYSROOT_UPGRADER, RpmOstreeSysrootUpgrader))
#define RPMOSTREE_IS_SYSROOT_UPGRADER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), RPMOSTREE_TYPE_SYSROOT_UPGRADER))

typedef struct RpmOstreeSysrootUpgrader RpmOstreeSysrootUpgrader;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmOstreeSysrootUpgrader, g_object_unref)

/**
 * RpmOstreeSysrootUpgraderFlags:
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_NONE: No options
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED: Do not error if the origin has an unconfigured-state key
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER: Do not error if the new deployment was composed earlier than the current deployment
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN: Don't deploy new base. If layering packages, only print the transaction
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGCACHE_ONLY: Don't try to update cached packages.
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_SYNTHETIC_PULL: Don't actually pull, just resolve ref and timestamp check
 * @RPMOSTREE_SYSROOT_UPGRADER_FLAGS_LOCK_FINALIZATION: Prevent deployment finalization on shutdown
 *
 * Flags controlling operation of an #RpmOstreeSysrootUpgrader.
 */
typedef enum {
  /* NB: When changing here, also change rpmostree_sysroot_upgrader_flags_get_type(). */
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_NONE                 = (1 << 0),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED  = (1 << 1),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER          = (1 << 2),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN              = (1 << 3),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGCACHE_ONLY        = (1 << 4),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_SYNTHETIC_PULL       = (1 << 5),
  RPMOSTREE_SYSROOT_UPGRADER_FLAGS_LOCK_FINALIZATION    = (1 << 6),
} RpmOstreeSysrootUpgraderFlags;

/* _NONE means we're doing pure ostree, no client-side computation.
 * _LOCAL is just e.g. rpm-ostree initramfs
 * _RPMMD_REPOS is where we're downloading data from /etc/yum.repos.d
 */
typedef enum {
  RPMOSTREE_SYSROOT_UPGRADER_LAYERING_NONE = 0,
  RPMOSTREE_SYSROOT_UPGRADER_LAYERING_LOCAL = 1,
  RPMOSTREE_SYSROOT_UPGRADER_LAYERING_RPMMD_REPOS = 2,
} RpmOstreeSysrootUpgraderLayeringType;

GType rpmostree_sysroot_upgrader_get_type (void);

GType rpmostree_sysroot_upgrader_flags_get_type (void);

RpmOstreeSysrootUpgrader *rpmostree_sysroot_upgrader_new (OstreeSysroot              *sysroot,
                                                          const char                 *osname,
                                                          RpmOstreeSysrootUpgraderFlags  flags,
                                                          GCancellable               *cancellable,
                                                          GError                    **error);

void rpmostree_sysroot_upgrader_set_caller_info (RpmOstreeSysrootUpgrader *self, 
                                                 const char               *initiating_command_line, 
                                                 const char               *agent,
                                                 const char               *sd_unit);

OstreeDeployment* rpmostree_sysroot_upgrader_get_merge_deployment (RpmOstreeSysrootUpgrader *self);

RpmOstreeOrigin *
rpmostree_sysroot_upgrader_dup_origin (RpmOstreeSysrootUpgrader *self);

void
rpmostree_sysroot_upgrader_set_origin (RpmOstreeSysrootUpgrader *self,
                                       RpmOstreeOrigin *origin);

const char *
rpmostree_sysroot_upgrader_get_base (RpmOstreeSysrootUpgrader *self);

DnfSack*
rpmostree_sysroot_upgrader_get_sack (RpmOstreeSysrootUpgrader *self,
                                     GError                  **error);

gboolean
rpmostree_sysroot_upgrader_pull_base (RpmOstreeSysrootUpgrader  *self,
                                      const char             *dir_to_pull,
                                      OstreeRepoPullFlags     flags,
                                      OstreeAsyncProgress    *progress,
                                      gboolean               *out_changed,
                                      GCancellable           *cancellable,
                                      GError                **error);

gboolean
rpmostree_sysroot_upgrader_prep_layering (RpmOstreeSysrootUpgrader *self,
                                          RpmOstreeSysrootUpgraderLayeringType *out_layering,
                                          gboolean                 *out_changed,
                                          GCancellable             *cancellable,
                                          GError                  **error);

gboolean
rpmostree_sysroot_upgrader_import_pkgs (RpmOstreeSysrootUpgrader *self,
                                        GCancellable             *cancellable,
                                        GError                  **error);

gboolean
rpmostree_sysroot_upgrader_pull_repos (RpmOstreeSysrootUpgrader  *self,
                                       const char             *dir_to_pull,
                                       OstreeRepoPullFlags     flags,
                                       OstreeAsyncProgress    *progress,
                                       gboolean               *out_changed,
                                       GCancellable           *cancellable,
                                       GError                **error);



gboolean
rpmostree_sysroot_upgrader_deploy (RpmOstreeSysrootUpgrader *self,
                                   OstreeDeployment        **out_deployment,
                                   GCancellable             *cancellable,
                                   GError                  **error);

void
rpmostree_sysroot_upgrader_set_kargs (RpmOstreeSysrootUpgrader *self,
                                      char                    **kernel_args);
G_END_DECLS
