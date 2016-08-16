/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
#include <sys/types.h>
#include <sys/wait.h>

gboolean
rpmostree_check_passwd (OstreeRepo      *repo,
                        GFile           *yumroot,
                        GFile           *treefile_path,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error);

gboolean
rpmostree_check_groups (OstreeRepo      *repo,
                        GFile           *yumroot,
                        GFile           *treefile_path,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error);

typedef enum {
  RPM_OSTREE_PASSWD_MIGRATE_PASSWD,
  RPM_OSTREE_PASSWD_MIGRATE_GROUP
} RpmOstreePasswdMigrateKind;

gboolean
rpmostree_passwd_migrate_except_root (GFile         *rootfs,
                                      RpmOstreePasswdMigrateKind    kind,
                                      GHashTable    *preserve,
                                      GCancellable  *cancellable,
                                      GError       **error);

gboolean
rpmostree_generate_passwd_from_previous (OstreeRepo      *repo,
                                         GFile           *yumroot,
                                         GFile           *treefile_dirpath,
                                         GFile           *previous_root,
                                         JsonObject      *treedata,
                                         GCancellable    *cancellable,
                                         GError         **error);

gboolean
rpmostree_passwd_prepare_rpm_layering (int       rootfs_dfd,
                                       GCancellable      *cancellable,
                                       GError  **error);

gboolean
rpmostree_passwd_complete_rpm_layering (int       rootfs_dfd,
                                        GError  **error);
