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
#include <libhif.h>
#include <libhif/hif-utils.h>
#include <libhif/hif-package.h>
#include <ostree.h>

#include "libglnx.h"

struct RpmOstreeHifInstall {
  GPtrArray *packages_requested;
  /* Target state */
  GPtrArray *packages_to_download;
  guint64 n_bytes_to_fetch;

  /* Current state */
  guint n_packages_fetched;
  guint64 n_bytes_fetched;
};

typedef struct RpmOstreeHifInstall RpmOstreeHifInstall;

struct RpmOstreePackageDownloadMetrics {
  guint64 bytes;
};

typedef struct RpmOstreePackageDownloadMetrics RpmOstreePackageDownloadMetrics;

HifContext *_rpmostree_libhif_new_default (void);

HifContext *_rpmostree_libhif_new (int rpmmd_cache_dfd,
                                   const char *installroot,
                                   const char *repos_dir,
                                   const char *const *enabled_repos,
                                   GCancellable *cancellable,
                                   GError **error);

void _rpmostree_reset_rpm_sighandlers (void);

void _rpmostree_libhif_set_cache_dfd (HifContext *hifctx, int dfd);

rpmts _rpmostree_libhif_ts_new (HifContext *hifctx);

gboolean _rpmostree_libhif_setup (HifContext    *context,
                                  GCancellable  *cancellable,
                                  GError       **error);

void _rpmostree_libhif_repos_disable_all (HifContext    *context);

void _rpmostree_libhif_set_ostree_repo (HifContext *context);

void _rpmostree_hif_add_checksum_goal (GChecksum *checksum, HyGoal goal);
char * _rpmostree_hif_checksum_goal (GChecksumType type, HyGoal goal);

char *_rpmostree_get_cache_branch_header (Header hdr);
char *_rpmostree_get_cache_branch_pkg (HyPackage pkg);

gboolean _rpmostree_libhif_repos_enable_by_name (HifContext    *context,
                                                 const char    *name,
                                                 GError       **error);

gboolean _rpmostree_libhif_console_download_metadata (HifContext     *context,
                                                      GCancellable   *cancellable,
                                                      GError        **error);

/* This API allocates an install context, use with one of the later ones */
gboolean _rpmostree_libhif_console_prepare_install (HifContext     *context,
                                                    OstreeRepo     *repo,
                                                    const char *const *packages,
                                                    struct RpmOstreeHifInstall *out_install,
                                                    GCancellable   *cancellable,
                                                    GError        **error);

gboolean _rpmostree_libhif_console_download_rpms (HifContext     *context,
                                                  int             target_dfd,
                                                  struct RpmOstreeHifInstall *install,
                                                  GCancellable   *cancellable,
                                                  GError        **error);

gboolean _rpmostree_libhif_console_download_import (HifContext                 *context,
                                                    OstreeRepo                 *repo,
                                                    struct RpmOstreeHifInstall *install,
                                                    GCancellable               *cancellable,
                                                    GError                    **error);

gboolean _rpmostree_libhif_console_assemble_commit (HifContext                 *context,
                                                    int                         tmpdir_dfd,
                                                    OstreeRepo                 *ostreerepo,
                                                    const char                 *name,
                                                    struct RpmOstreeHifInstall *install,
                                                    char                      **out_commit,
                                                    GCancellable               *cancellable,
                                                    GError                    **error);

static inline void
_rpmostree_hif_install_cleanup (struct RpmOstreeHifInstall *hifinst)
{
  g_clear_pointer (&hifinst->packages_requested, g_ptr_array_unref);
  g_clear_pointer (&hifinst->packages_to_download, g_ptr_array_unref);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(RpmOstreeHifInstall, _rpmostree_hif_install_cleanup)
