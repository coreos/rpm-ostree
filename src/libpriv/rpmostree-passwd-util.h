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
#include <json-glib/json-glib.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "rpmostree-cxxrs.h"
#include "rpmostree-rust.h"

G_BEGIN_DECLS

gboolean
rpmostree_check_passwd (OstreeRepo      *repo,
                        int              rootfs_dfd,
                        rpmostreecxx::Treefile &treefile_rs,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error);

gboolean
rpmostree_check_groups (OstreeRepo      *repo,
                        int              rootfs_dfd,
                        rpmostreecxx::Treefile &treefile_rs,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error);

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

G_END_DECLS
