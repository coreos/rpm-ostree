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
#include <memory>

#include "rpmostree-cxxrs.h"
#include "libglnx.h"


// C++ APIs
std::unique_ptr<rust::Vec<rpmostreecxx::StringMapping>> rpmostree_dnfcontext_get_varsubsts (DnfContext *context);

// Begin C APIs
G_BEGIN_DECLS

#define RPMOSTREE_CORE_CACHEDIR "/var/cache/rpm-ostree/"
#define RPMOSTREE_DIR_CACHE_REPOMD "repomd"
#define RPMOSTREE_DIR_CACHE_SOLV "solv"
#define RPMOSTREE_DIR_LOCK "lock"

/* See http://lists.rpm.org/pipermail/rpm-maint/2017-October/006681.html */
/* This is also defined on the Rust side. */
#define RPMOSTREE_RPMDB_LOCATION "usr/share/rpm"
#define RPMOSTREE_SYSIMAGE_DIR "usr/lib/sysimage"
#define RPMOSTREE_SYSIMAGE_RPMDB RPMOSTREE_SYSIMAGE_DIR "/rpm"
#define RPMOSTREE_BASE_RPMDB RPMOSTREE_SYSIMAGE_DIR "/rpm-ostree-base-db"

/* put it in cache dir so it gets destroyed naturally with a `cleanup -m` */
#define RPMOSTREE_AUTOUPDATES_CACHE_FILE RPMOSTREE_CORE_CACHEDIR "cached-update.gv"

#define RPMOSTREE_STATE_DIR "/var/lib/rpm-ostree/"
#define RPMOSTREE_HISTORY_DIR RPMOSTREE_STATE_DIR "history"

#define RPMOSTREE_TYPE_CONTEXT (rpmostree_context_get_type ())
G_DECLARE_FINAL_TYPE (RpmOstreeContext, rpmostree_context, RPMOSTREE, CONTEXT, GObject)

#define RPMOSTREE_TYPE_TREESPEC (rpmostree_treespec_get_type ())
G_DECLARE_FINAL_TYPE (RpmOstreeTreespec, rpmostree_treespec, RPMOSTREE, TREESPEC, GObject)

/* Now in the code we handle "refspec" types of rojig (rpm-ostree jigdo),
 * in addition to ostree.
 */
typedef enum {
  RPMOSTREE_REFSPEC_TYPE_OSTREE,
  RPMOSTREE_REFSPEC_TYPE_ROJIG,
  RPMOSTREE_REFSPEC_TYPE_CHECKSUM,
} RpmOstreeRefspecType;

#define RPMOSTREE_REFSPEC_OSTREE_PREFIX "ostree://"
#define RPMOSTREE_REFSPEC_ROJIG_PREFIX "rojig://"

gboolean rpmostree_refspec_classify (const char *refspec,
                                     RpmOstreeRefspecType *out_type,
                                     const char **out_remainder,
                                     GError     **error);

char* rpmostree_refspec_to_string (RpmOstreeRefspecType  reftype,
                                   const char           *data);

char* rpmostree_refspec_canonicalize (const char           *orig_refspec,
                                      GError              **error);

RpmOstreeContext *rpmostree_context_new_system (OstreeRepo   *repo,
                                                GCancellable *cancellable,
                                                GError      **error);

RpmOstreeContext *rpmostree_context_new_tree (int basedir_dfd,
                                              OstreeRepo  *repo,
                                              GCancellable *cancellable,
                                              GError **error);

void rpmostree_context_set_pkgcache_only (RpmOstreeContext *self,
                                          gboolean          pkgcache_only);

typedef enum {
      RPMOSTREE_CONTEXT_DNF_CACHE_FOREVER,
      RPMOSTREE_CONTEXT_DNF_CACHE_DEFAULT,
      RPMOSTREE_CONTEXT_DNF_CACHE_NEVER,
} RpmOstreeContextDnfCachePolicy;

void rpmostree_context_set_dnf_caching (RpmOstreeContext *self,
                                        RpmOstreeContextDnfCachePolicy policy);

DnfContext * rpmostree_context_get_dnf (RpmOstreeContext *self);

RpmOstreeTreespec *rpmostree_treespec_new_from_keyfile (GKeyFile *keyfile, GError  **error);
RpmOstreeTreespec *rpmostree_treespec_new_from_path (const char *path, GError  **error);
RpmOstreeTreespec *rpmostree_treespec_new (GVariant   *variant);

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

void rpmostree_context_set_treefile (RpmOstreeContext *self, rpmostreecxx::Treefile *treefile_rs);

void rpmostree_context_set_is_empty (RpmOstreeContext *self);

void rpmostree_context_set_repos (RpmOstreeContext *self,
                                  OstreeRepo       *base_repo,
                                  OstreeRepo       *pkgcache_repo);
void rpmostree_context_set_devino_cache (RpmOstreeContext *self,
                                         OstreeRepoDevInoCache *devino_cache);
void rpmostree_context_disable_rofiles (RpmOstreeContext *self);
void rpmostree_context_set_sepolicy (RpmOstreeContext *self,
                                     OstreeSePolicy   *sepolicy);

gboolean rpmostree_dnf_add_checksum_goal (GChecksum  *checksum,
                                          HyGoal      goal,
                                          OstreeRepo *pkgcache_repo,
                                          GError    **error);

gboolean rpmostree_context_get_state_sha512 (RpmOstreeContext *self,
                                             char            **out_checksum,
                                             GError          **error);

gboolean
rpmostree_pkgcache_find_pkg_header (OstreeRepo    *pkgcache,
                                    const char    *nevra,
                                    const char    *expected_sha256,
                                    GVariant     **out_header,
                                    GCancellable  *cancellable,
                                    GError       **error);

gboolean rpmostree_context_download_metadata (RpmOstreeContext  *context,
                                              DnfContextSetupSackFlags flags,
                                              GCancellable      *cancellable,
                                              GError           **error);

/* This API allocates an install context, use with one of the later ones */
gboolean rpmostree_context_prepare (RpmOstreeContext     *self,
                                    GCancellable   *cancellable,
                                    GError        **error);
/* Like above, but used for "pure rojig" cases */
gboolean rpmostree_context_prepare_rojig (RpmOstreeContext     *self,
                                          gboolean              allow_not_found,
                                          GCancellable         *cancellable,
                                          GError              **error);

GPtrArray *rpmostree_context_get_packages (RpmOstreeContext *self);

/* Alternative to _prepare() for non-depsolve cases like rojig */
gboolean rpmostree_context_set_packages (RpmOstreeContext *self,
                                         GPtrArray        *packages,
                                         GCancellable     *cancellable,
                                         GError          **error);

GPtrArray *rpmostree_context_get_packages_to_import (RpmOstreeContext *self);

void
rpmostree_context_set_vlockmap (RpmOstreeContext *self,
                                GHashTable       *map,
                                gboolean          strict);

gboolean rpmostree_download_packages (GPtrArray      *packages,
                                      GCancellable   *cancellable,
                                      GError        **error);

gboolean rpmostree_context_download (RpmOstreeContext *self,
                                     GCancellable     *cancellable,
                                     GError           **error);

void rpmostree_set_repos_on_packages (DnfContext *dnfctx,
                                      GPtrArray  *packages);

gboolean rpmostree_context_execute_rojig (RpmOstreeContext     *self,
                                          gboolean             *out_changed,
                                          GCancellable         *cancellable,
                                          GError              **error);

gboolean
rpmostree_context_consume_package (RpmOstreeContext  *self,
                                   DnfPackage        *package,
                                   int               *out_fd,
                                   GError           **error);

DnfPackage *rpmostree_context_get_rojig_pkg (RpmOstreeContext  *self);
const char *rpmostree_context_get_rojig_checksum (RpmOstreeContext  *self);
const char *rpmostree_context_get_rojig_inputhash (RpmOstreeContext  *self);

gboolean rpmostree_context_import (RpmOstreeContext *self,
                                   GCancellable     *cancellable,
                                   GError          **error);

gboolean rpmostree_context_import_rojig (RpmOstreeContext *self,
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

gboolean rpmostree_context_get_kernel_changed (RpmOstreeContext *self);

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

G_END_DECLS
