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
                        int              rootfs_dfd,
                        GFile           *treefile_path,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error);

gboolean
rpmostree_check_groups (OstreeRepo      *repo,
                        int              rootfs_dfd,
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
rpmostree_passwd_migrate_except_root (int            rootfs_dfd,
                                      RpmOstreePasswdMigrateKind    kind,
                                      GHashTable    *preserve,
                                      GCancellable  *cancellable,
                                      GError       **error);

gboolean
rpmostree_generate_passwd_from_previous (OstreeRepo      *repo,
                                         int              rootfs_dfd,
                                         const char      *dest,
                                         GFile           *treefile_dirpath,
                                         GFile           *previous_root,
                                         JsonObject      *treedata,
                                         GCancellable    *cancellable,
                                         GError         **error);

typedef struct RpmOstreePasswdDB RpmOstreePasswdDB;
RpmOstreePasswdDB *
rpmostree_passwddb_open (int rootfs, GCancellable *cancellable, GError **error);
const char *
rpmostree_passwddb_lookup_user (RpmOstreePasswdDB *db, uid_t uid);
const char *
rpmostree_passwddb_lookup_group (RpmOstreePasswdDB *db, gid_t gid);
void
rpmostree_passwddb_free (RpmOstreePasswdDB *db);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreePasswdDB, rpmostree_passwddb_free)

gboolean
rpmostree_passwd_cleanup (int rootfs_dfd, GCancellable *cancellable, GError **error);

gboolean
rpmostree_passwd_prepare_rpm_layering (int       rootfs_dfd,
                                       const char        *merge_passwd_dir,
                                       gboolean          *out_have_usrlib_passwd,
                                       GCancellable      *cancellable,
                                       GError  **error);

gboolean
rpmostree_passwd_complete_rpm_layering (int       rootfs_dfd,
                                        GError  **error);

struct sysuser_ent {
  const char *type; /* type of sysuser entry, can be 1: u (user) 2: g (group) 3: m (mixed) 4: r (ranged ids) */
  char *name;
  char *id;         /* id used by sysuser entry, can be in the form of 1: uid 2:gid 3:uid:gid */
  char *gecos;      /* user information */
  char *dir;        /* home directory */
  char *shell;      /* login shell, default to /sbin/nologin */
};

struct conv_passwd_ent {
  char *name;
  uid_t uid;
  gid_t gid;
  char *pw_gecos;   /* user information */
  char *pw_dir;     /* home directory */
  char *pw_shell;   /* login shell */
};

struct conv_group_ent {
  char *name;
  gid_t gid;
};

GPtrArray *
rpmostree_passwd_data2passwdents (const char *data);

GPtrArray *
rpmostree_passwd_data2groupents (const char *data);

gboolean
rpmostree_passwdents2sysusers (GPtrArray *passwd_ents,
                               GPtrArray **out_sysusers_entries,
                               GError **error);
gboolean
rpmostree_groupents2sysusers (GPtrArray  *group_ents,
                              GPtrArray **out_sysusers_entries,
                              GError    **error);
