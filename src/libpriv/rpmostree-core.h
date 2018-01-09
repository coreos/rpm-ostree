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
/* See http://lists.rpm.org/pipermail/rpm-maint/2017-October/006681.html */
#define RPMOSTREE_RPMDB_LOCATION "usr/share/rpm"
#define RPMOSTREE_SYSIMAGE_DIR "usr/lib/sysimage"
#define RPMOSTREE_BASE_RPMDB RPMOSTREE_SYSIMAGE_DIR "/rpm-ostree-base-db"

#define RPMOSTREE_TYPE_CONTEXT (rpmostree_context_get_type ())
G_DECLARE_FINAL_TYPE (RpmOstreeContext, rpmostree_context, RPMOSTREE, CONTEXT, GObject)

#define RPMOSTREE_TYPE_TREESPEC (rpmostree_treespec_get_type ())
G_DECLARE_FINAL_TYPE (RpmOstreeTreespec, rpmostree_treespec, RPMOSTREE, TREESPEC, GObject)

RpmOstreeContext *rpmostree_context_new_system (OstreeRepo   *repo,
                                                GCancellable *cancellable,
                                                GError      **error);

RpmOstreeContext *rpmostree_context_new_tree (int basedir_dfd,
                                              OstreeRepo  *repo,
                                              GCancellable *cancellable,
                                              GError **error);

void rpmostree_context_set_pkgcache_only (RpmOstreeContext *self,
                                          gboolean          pkgcache_only);

DnfContext * rpmostree_context_get_dnf (RpmOstreeContext *self);

RpmOstreeTreespec *rpmostree_treespec_new_from_keyfile (GKeyFile *keyfile, GError  **error);
RpmOstreeTreespec *rpmostree_treespec_new_from_path (const char *path, GError  **error);
RpmOstreeTreespec *rpmostree_treespec_new (GVariant   *variant);

GHashTable *rpmostree_dnfcontext_get_varsubsts (DnfContext *context);

GVariant *rpmostree_context_get_rpmmd_repo_commit_metadata (RpmOstreeContext  *self);

GVariant *rpmostree_treespec_to_variant (RpmOstreeTreespec *spec);
const char *rpmostree_treespec_get_ref (RpmOstreeTreespec *spec);

gboolean rpmostree_context_setup (RpmOstreeContext     *self,
                                  const char    *install_root,
                                  const char    *source_root,
                                  RpmOstreeTreespec *treespec,
                                  GCancellable  *cancellable,
                                  GError       **error);

void
rpmostree_context_configure_from_deployment (RpmOstreeContext *self,
                                             OstreeSysroot    *sysroot,
                                             OstreeDeployment *cfg_deployment);

void rpmostree_context_set_is_empty (RpmOstreeContext *self);

void rpmostree_context_set_repos (RpmOstreeContext *self,
                                  OstreeRepo       *base_repo,
                                  OstreeRepo       *pkgcache_repo);
void rpmostree_context_set_devino_cache (RpmOstreeContext *self,
                                         OstreeRepoDevInoCache *devino_cache);
void rpmostree_context_set_sepolicy (RpmOstreeContext *self,
                                     OstreeSePolicy   *sepolicy);

gboolean rpmostree_dnf_add_checksum_goal (GChecksum  *checksum,
                                          HyGoal      goal,
                                          OstreeRepo *pkgcache_repo,
                                          GError    **error);

gboolean rpmostree_context_get_state_sha512 (RpmOstreeContext *self,
                                             char            **out_checksum,
                                             GError          **error);

char * rpmostree_get_cache_branch_for_n_evr_a (const char *name, const char *evr, const char *arch);
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
/* Like above, but used for "pure jigdo" cases */
gboolean rpmostree_context_prepare_jigdo (RpmOstreeContext     *self,
                                          GCancellable   *cancellable,
                                          GError        **error);

GPtrArray *rpmostree_context_get_packages (RpmOstreeContext *self);

/* Alternative to _prepare() for non-depsolve cases like jigdo */
gboolean rpmostree_context_set_packages (RpmOstreeContext *self,
                                         GPtrArray        *packages,
                                         GCancellable     *cancellable,
                                         GError          **error);

GPtrArray *rpmostree_context_get_packages_to_import (RpmOstreeContext *self);

gboolean rpmostree_context_download (RpmOstreeContext *self,
                                     GCancellable     *cancellable,
                                     GError           **error);

gboolean rpmostree_context_execute_jigdo (RpmOstreeContext     *self,
                                          gboolean             *out_changed,
                                          GCancellable         *cancellable,
                                          GError              **error);

gboolean
rpmostree_context_consume_package (RpmOstreeContext  *self,
                                   DnfPackage        *package,
                                   int               *out_fd,
                                   GError           **error);

DnfPackage *rpmostree_context_get_jigdo_pkg (RpmOstreeContext  *self);
const char *rpmostree_context_get_jigdo_checksum (RpmOstreeContext  *self);

gboolean rpmostree_context_import (RpmOstreeContext *self,
                                   GCancellable     *cancellable,
                                   GError          **error);

gboolean rpmostree_context_import_jigdo (RpmOstreeContext *self,
                                         GVariant         *xattr_table,
                                         GHashTable       *pkg_to_xattrs,
                                         GCancellable     *cancellable,
                                         GError          **error);

gboolean rpmostree_context_force_relabel (RpmOstreeContext *self,
                                          GCancellable     *cancellable,
                                          GError          **error);

typedef enum {
  RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE,
  RPMOSTREE_ASSEMBLE_TYPE_CLIENT_LAYERING
} RpmOstreeAssembleType;

void rpmostree_context_set_tmprootfs_dfd (RpmOstreeContext *self,
                                          int               dfd);
int rpmostree_context_get_tmprootfs_dfd  (RpmOstreeContext *self);

/* NB: tmprootfs_dfd is allowed to have pre-existing data */
/* devino_cache can be NULL if no previous cache established */
gboolean rpmostree_context_assemble (RpmOstreeContext      *self,
                                     GCancellable          *cancellable,
                                     GError               **error);
gboolean rpmostree_context_commit (RpmOstreeContext      *self,
                                   const char            *parent,
                                   RpmOstreeAssembleType  assemble_type,
                                   char                 **out_commit,
                                   GCancellable          *cancellable,
                                   GError               **error);
