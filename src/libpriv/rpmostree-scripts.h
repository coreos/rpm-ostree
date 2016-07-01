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
#include <libhif/libhif.h>

#include "libglnx.h"

typedef enum {
  RPMOSTREE_SCRIPT_ACTION_DEFAULT = 0,
  RPMOSTREE_SCRIPT_ACTION_IGNORE,
  RPMOSTREE_SCRIPT_ACTION_TODO_SHELL_POSTTRANS = RPMOSTREE_SCRIPT_ACTION_IGNORE,
} RpmOstreeScriptAction;

struct RpmOstreePackageScriptHandler {
  const char *package_script;
  RpmOstreeScriptAction action;
};

const struct RpmOstreePackageScriptHandler* rpmostree_script_gperf_lookup(const char *key, unsigned length);

gboolean rpmostree_script_ignore_hash_from_strv (const char *const *strv,
                                                 GHashTable **out_hash,
                                                 GError **error);

gboolean
rpmostree_script_txn_validate (HifPackage    *package,
                               Header         hdr,
                               GHashTable    *ignore_scripts,
                               GCancellable  *cancellable,
                               GError       **error);

gboolean
rpmostree_posttrans_run_sync (HifPackage    *pkg,
                              Header         hdr,
                              GHashTable    *ignore_scripts,
                              int            rootfs_fd,
                              GCancellable  *cancellable,
                              GError       **error);

gboolean
rpmostree_run_script_container (int rootfs_fd,
                                const char *scriptdesc,
                                const char *script,
                                const char *const *argv,
                                GCancellable  *cancellable,
                                GError       **error);

gboolean
rpmostree_run_script_localedef (int rootfs_fd,
                                const char *const *instlangs,
                                GCancellable  *cancellable,
                                GError       **error);
