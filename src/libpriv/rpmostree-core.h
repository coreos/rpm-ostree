/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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
#include <libdnf/libdnf.h>
#include <ostree.h>

#include "libglnx.h"

#define RPMOSTREE_CORE_CACHEDIR "/var/cache/rpm-ostree/"

#define RPMOSTREE_TYPE_CONTEXT (rpmostree_context_get_type ())
G_DECLARE_FINAL_TYPE (RpmOstreeContext, rpmostree_context, RPMOSTREE, CONTEXT, GObject)

#define RPMOSTREE_TYPE_TREESPEC (rpmostree_treespec_get_type ())
G_DECLARE_FINAL_TYPE (RpmOstreeTreespec, rpmostree_treespec, RPMOSTREE, TREESPEC, GObject)

RpmOstreeContext *rpmostree_context_new_system (GCancellable *cancellable,
                                                GError **error);

RpmOstreeContext *rpmostree_context_new_compose (int basedir_dfd,
                                                 GCancellable *cancellable,
                                                 GError **error);

RpmOstreeContext *rpmostree_context_new_unprivileged (int basedir_dfd,
                                                      GCancellable *cancellable,
                                                      GError **error);

DnfContext * rpmostree_context_get_hif (RpmOstreeContext *self);

RpmOstreeTreespec *rpmostree_treespec_new_from_keyfile (GKeyFile *keyfile, GError  **error);
RpmOstreeTreespec *rpmostree_treespec_new_from_path (const char *path, GError  **error);
RpmOstreeTreespec *rpmostree_treespec_new (GVariant   *variant);

GHashTable *rpmostree_context_get_varsubsts (RpmOstreeContext *context);

GVariant *rpmostree_treespec_to_variant (RpmOstreeTreespec *spec);
const char *rpmostree_treespec_get_ref (RpmOstreeTreespec *spec);

gboolean rpmostree_context_setup (RpmOstreeContext     *self,
                                  const char    *install_root,
                                  const char    *source_root,
                                  RpmOstreeTreespec *treespec,
                                  GCancellable  *cancellable,
                                  GError       **error);

void rpmostree_context_set_is_empty (RpmOstreeContext *self);

void rpmostree_context_set_repos (RpmOstreeContext *self,
                                  OstreeRepo       *base_repo,
                                  OstreeRepo       *pkgcache_repo);
void rpmostree_context_set_sepolicy (RpmOstreeContext *self,
                                     OstreeSePolicy   *sepolicy);
void rpmostree_context_set_passwd_dir (RpmOstreeContext *self,
                                       const char *passwd_dir);

void rpmostree_dnf_add_checksum_goal (GChecksum *checksum, HyGoal goal);
char *rpmostree_context_get_state_sha512 (RpmOstreeContext *self);

char *rpmostree_get_cache_branch_header (Header hdr);
char *rpmostree_get_cache_branch_pkg (DnfPackage *pkg);

gboolean
rpmostree_find_cache_branch_by_nevra (OstreeRepo    *pkgcache,
                                      const char    *nevra,
                                      char         **out_cache_branch,
                                      GCancellable  *cancellable,
                                      GError       **error);

gboolean
rpmostree_pkgcache_find_pkg_header (OstreeRepo    *pkgcache,
                                    const char    *nevra,
                                    const char    *expected_sha256,
                                    GVariant     **out_header,
                                    GCancellable  *cancellable,
                                    GError       **error);

gboolean
rpmostree_get_nevra_from_pkgcache (OstreeRepo   *repo,
                                   const char   *nevra,
                                   char        **out_name,
                                   guint64      *out_epoch,
                                   char        **out_version,
                                   char        **out_release,
                                   char        **out_arch,
                                   GCancellable *cancellable,
                                   GError  **error);

gboolean rpmostree_context_download_metadata (RpmOstreeContext  *context,
                                               GCancellable      *cancellable,
                                               GError           **error);

/* This API allocates an install context, use with one of the later ones */
gboolean rpmostree_context_prepare (RpmOstreeContext     *self,
                                    GCancellable   *cancellable,
                                    GError        **error);

gboolean rpmostree_context_download (RpmOstreeContext *self,
                                     GCancellable     *cancellable,
                                     GError           **error);

gboolean rpmostree_context_import (RpmOstreeContext *self,
                                   GCancellable     *cancellable,
                                   GError          **error);

gboolean rpmostree_context_relabel (RpmOstreeContext *self,
                                    GCancellable     *cancellable,
                                    GError          **error);

typedef enum {
  RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE,
  RPMOSTREE_ASSEMBLE_TYPE_CLIENT_LAYERING
} RpmOstreeAssembleType;

/* NB: tmprootfs_dfd is allowed to have pre-existing data */
/* devino_cache can be NULL if no previous cache established */
gboolean rpmostree_context_assemble_tmprootfs (RpmOstreeContext      *self,
                                               int                    tmprootfs_dfd,
                                               OstreeRepoDevInoCache *devino_cache,
                                               gboolean               noscripts,
                                               GCancellable          *cancellable,
                                               GError               **error);
gboolean rpmostree_context_commit_tmprootfs (RpmOstreeContext      *self,
                                             int                    tmprootfs_dfd,
                                             OstreeRepoDevInoCache *devino_cache,
                                             const char            *parent,
                                             RpmOstreeAssembleType  assemble_type,
                                             char                 **out_commit,
                                             GCancellable          *cancellable,
                                             GError               **error);
/* Wrapper for both of the above */
gboolean rpmostree_context_assemble_commit (RpmOstreeContext      *self,
                                            int                    tmprootfs_dfd,
                                            OstreeRepoDevInoCache *devino_cache,
                                            const char            *parent,
                                            RpmOstreeAssembleType  assemble_type,
                                            gboolean               noscripts,
                                            char                 **out_commit,
                                            GCancellable          *cancellable,
                                            GError               **error);
