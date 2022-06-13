/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Anne LoVerso <anne.loverso@students.olin.edu>
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

#include <memory>
#include <ostree.h>

#include "libglnx.h"
#include "rpmostree-refsack.h"
#include "rpmostree-refts.h"
#include "rpmostree-util.h"
#include <rpm/rpmdb.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>

#include "libglnx.h"

// C++ code
namespace rpmostreecxx
{
rust::String nevra_to_cache_branch (const std::string &nevra);
rust::String get_repodata_chksum_repr (DnfPackage &pkg);
std::unique_ptr<RpmTs> rpmts_for_commit (const OstreeRepo &repo, rust::Str rev);
rust::Vec<rust::String> rpmdb_package_name_list (gint32 dfd, rust::String path);
}

// C code follows
G_BEGIN_DECLS

struct RpmHeaders
{
  RpmOstreeRefTs *refts; /* rpm transaction set the headers belong to */
  GPtrArray *hs;         /* list of rpm header objects from <rpm.h> = Header */
};

typedef struct RpmHeaders RpmHeaders;

struct RpmHeadersDiff
{
  GPtrArray *hs_add;     /* list of rpm header objects from <rpm.h> = Header */
  GPtrArray *hs_del;     /* list of rpm header objects from <rpm.h> = Header */
  GPtrArray *hs_mod_old; /* list of rpm header objects from <rpm.h> = Header */
  GPtrArray *hs_mod_new; /* list of rpm header objects from <rpm.h> = Header */
};

typedef struct RpmRevisionData RpmRevisionData;

struct RpmHeadersDiff *rpmhdrs_diff (struct RpmHeaders *l1, struct RpmHeaders *l2);

void rpmhdrs_list (struct RpmHeaders *l1);

char *rpmhdrs_rpmdbv (struct RpmHeaders *l1, GCancellable *cancellable, GError **error);

void rpmhdrs_diff_prnt_block (gboolean changelogs, struct RpmHeadersDiff *diff);

/* Define cleanup functions for librpm here. Note that this
 * will break if one day librpm ever decides to define these
 * itself. TODO: Move them to libdnf */
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (FD_t, Fclose, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (Header, headerFree, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (rpmfi, rpmfiFree, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (rpmts, rpmtsFree, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (rpmdbMatchIterator, rpmdbFreeIterator, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (rpmfiles, rpmfilesFree, NULL)

void rpmhdrs_diff_prnt_diff (struct RpmHeadersDiff *diff);

struct RpmRevisionData *rpmrev_new (OstreeRepo *repo, const char *rev, const GPtrArray *patterns,
                                    GCancellable *cancellable, GError **error);

struct RpmHeaders *rpmrev_get_headers (struct RpmRevisionData *self);

const char *rpmrev_get_commit (struct RpmRevisionData *self);

void rpmrev_free (struct RpmRevisionData *ptr);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmRevisionData, rpmrev_free);

void rpmhdrs_free (RpmHeaders *hdrs);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmHeaders, rpmhdrs_free);

RpmOstreeRefSack *rpmostree_get_refsack_for_commit (OstreeRepo *repo, const char *ref,
                                                    GCancellable *cancellable, GError **error);

RpmOstreeRefSack *rpmostree_get_refsack_for_root (int dfd, const char *path, GError **error);

gboolean rpmostree_get_base_refsack_for_root (int dfd, const char *path,
                                              RpmOstreeRefSack **out_sack,
                                              GCancellable *cancellable, GError **error);

RpmOstreeRefSack *rpmostree_get_base_refsack_for_commit (OstreeRepo *repo, const char *ref,
                                                         GCancellable *cancellable, GError **error);

gboolean rpmostree_get_refts_for_commit (OstreeRepo *repo, const char *ref, RpmOstreeRefTs **out_ts,
                                         GCancellable *cancellable, GError **error);

GVariant *rpmostree_variant_pkgs_from_sack (RpmOstreeRefSack *sack);

gint rpmostree_pkg_array_compare (DnfPackage **p_pkg1, DnfPackage **p_pkg2);

void rpmostree_print_transaction (DnfContext *context);

GVariant *rpmostree_fcap_to_ostree_xattr (const char *fcap);
GVariant *rpmostree_fcap_to_xattr_variant (const char *fcap);

typedef enum
{
  PKG_NEVRA_FLAGS_NAME = (1 << 0),
  PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE = (1 << 1),
  PKG_NEVRA_FLAGS_EVR = PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE,
  PKG_NEVRA_FLAGS_VERSION_RELEASE = (1 << 2),
  PKG_NEVRA_FLAGS_ARCH = (1 << 3),
  PKG_NEVRA_FLAGS_NEVRA = (PKG_NEVRA_FLAGS_NAME | PKG_NEVRA_FLAGS_EVR | PKG_NEVRA_FLAGS_ARCH),
} RpmOstreePkgNevraFlags;

void rpmostree_custom_nevra (GString *buffer, const char *name, uint64_t epoch, const char *version,
                             const char *release, const char *arch, RpmOstreePkgNevraFlags flags);

char *rpmostree_custom_nevra_strdup (const char *name, uint64_t epoch, const char *version,
                                     const char *release, const char *arch,
                                     RpmOstreePkgNevraFlags flags);

char *rpmostree_header_custom_nevra_strdup (Header h, RpmOstreePkgNevraFlags flags);

GPtrArray *rpmostree_get_matching_packages (DnfSack *sack, const char *pattern);

gboolean rpmostree_sack_has_subject (DnfSack *sack, const char *pattern);

gboolean rpmostree_sack_get_by_pkgname (DnfSack *sack, const char *pkgname, DnfPackage **out_pkg,
                                        GError **error);

gboolean rpmostree_sack_has_pkgname (DnfSack *sack, const char *pkgname);

GPtrArray *rpmostree_sack_get_packages (DnfSack *sack);

GPtrArray *rpmostree_sack_get_sorted_packages (DnfSack *sack);

gboolean rpmostree_create_rpmdb_pkglist_variant (int dfd, const char *path, GVariant **out_variant,
                                                 GCancellable *cancellable, GError **error);

char *rpmostree_get_cache_branch_for_n_evr_a (const char *name, const char *evr, const char *arch);
char *rpmostree_get_cache_branch_header (Header hdr);
char *rpmostree_get_cache_branch_pkg (DnfPackage *pkg);

gboolean rpmostree_decompose_nevra (const char *nevra, char **out_name, /* allow-none */
                                    guint64 *out_epoch,                 /* allow-none */
                                    char **out_version,                 /* allow-none */
                                    char **out_release,                 /* allow-none */
                                    char **out_arch,                    /* allow-none */
                                    GError **error);

gboolean rpmostree_is_valid_nevra (const char *subject);

gboolean rpmostree_nevra_to_cache_branch (const char *nevra, char **cache_branch, GError **error);

GPtrArray *rpmostree_get_enabled_rpmmd_repos (DnfContext *dnfctx, DnfRepoEnabled enablement);

/*  s     advisory id (e.g. FEDORA-2018-a1b2c3d4e5f6)
    u     advisory kind (enum DnfAdvisoryKind)
    u     advisory severity (enum RpmOstreeAdvisorySeverity)
    as    list of packages (NEVRAs) contained in the advisory
    a{sv} additional info about advisory
      "cve_references" -> 'a(ss)'
        s   title
        s   URL

    This is also defined in utils.rs.
*/
#define RPMOSTREE_UPDATE_ADVISORY_GVARIANT_STRING "a(suuasa{sv})"
#define RPMOSTREE_UPDATE_ADVISORY_GVARIANT_FORMAT                                                  \
  G_VARIANT_TYPE (RPMOSTREE_UPDATE_ADVISORY_GVARIANT_STRING)

GVariant *rpmostree_advisories_variant (DnfSack *sack, GPtrArray *pkgs);

G_END_DECLS

namespace rpmostreecxx
{
rust::String header_get_nevra (Header h);
}
