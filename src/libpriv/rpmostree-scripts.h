/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <gio/gio.h>
#include <ostree.h>
#include <rpm/rpmsq.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmmacro.h>
#include <rpm/rpmts.h>
#include <libdnf/libdnf.h>

#include "libglnx.h"

G_BEGIN_DECLS

typedef enum {
  RPMOSTREE_SCRIPT_ACTION_DEFAULT = 0,
  RPMOSTREE_SCRIPT_ACTION_IGNORE,
  RPMOSTREE_SCRIPT_ACTION_TODO_SHELL_POSTTRANS = RPMOSTREE_SCRIPT_ACTION_IGNORE,
} RpmOstreeScriptAction;

struct RpmOstreePackageScriptHandler {
  const char *package_script;
  RpmOstreeScriptAction action;
};

const struct RpmOstreePackageScriptHandler* rpmostree_script_gperf_lookup(const char *key, GPERF_LEN_TYPE length);

typedef enum {
  RPMOSTREE_SCRIPT_PREIN,
  RPMOSTREE_SCRIPT_POSTIN,
  RPMOSTREE_SCRIPT_POSTTRANS,
} RpmOstreeScriptKind;

gboolean
rpmostree_script_txn_validate (DnfPackage    *package,
                               Header         hdr,
                               GCancellable  *cancellable,
                               GError       **error);

gboolean
rpmostree_script_run_sync (DnfPackage    *pkg,
                           Header         hdr,
                           RpmOstreeScriptKind kind,
                           int            rootfs_fd,
                           GLnxTmpDir    *var_lib_rpm_statedir,
                           gboolean       enable_rofiles,
                           guint         *out_n_run,
                           GCancellable  *cancellable,
                           GError       **error);

gboolean
rpmostree_transfiletriggers_run_sync (Header         hdr,
                                      int            rootfs_fd,
                                      gboolean       enable_rofiles,
                                      guint         *out_n_run,
                                      GCancellable  *cancellable,
                                      GError       **error);

gboolean
rpmostree_deployment_sanitycheck_true (int           rootfs_fd,
                                       GCancellable *cancellable,
                                       GError      **error);

gboolean
rpmostree_deployment_sanitycheck_rpmdb (int           rootfs_fd,
                                        GPtrArray     *overlays,
                                        GPtrArray     *overrides,
                                        GCancellable *cancellable,
                                        GError      **error);

G_END_DECLS
