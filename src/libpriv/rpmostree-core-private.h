/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
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

#include <optional>

#include "libglnx.h"
#include "rpmostree-core.h"
#include "rpmostree-output.h"
#include "rpmostree-cxxrs.h"

G_BEGIN_DECLS

struct _RpmOstreeContext {
  GObject parent;

  /* Whether we were created with new_system() or new_container() */
  gboolean is_system;
  /* Whether we were created with new_container() */
  gboolean is_container;
  std::optional<rust::Box<rpmostreecxx::Treefile>> treefile_owned;
  rpmostreecxx::Treefile *treefile_rs; /* For composes for now */
  gboolean empty;
  gboolean disable_selinux;
  char *ref;

  gboolean pkgcache_only;
  DnfContext *dnfctx;
  RpmOstreeContextDnfCachePolicy dnf_cache_policy;
  OstreeRepo *ostreerepo;
  OstreeRepo *pkgcache_repo;
  gboolean enable_rofiles;
  OstreeRepoDevInoCache *devino_cache;
  gboolean unprivileged;
  OstreeSePolicy *sepolicy;
  char *passwd_dir;

  guint async_index; /* Offset into array if applicable */
  guint n_async_running;
  guint n_async_max;
  gboolean async_running;
  GCancellable *async_cancellable;
  std::unique_ptr<rpmostreecxx::Progress> async_progress;
  GError *async_error;
  GPtrArray *pkgs; /* All packages */
  GPtrArray *pkgs_to_download;
  GPtrArray *pkgs_to_import;
  guint n_async_pkgs_imported;
  GPtrArray *pkgs_to_relabel;
  guint n_async_pkgs_relabeled;

  GHashTable *pkgs_to_remove;  /* pkgname --> gv_nevra */
  GHashTable *pkgs_to_replace; /* new gv_nevra --> old gv_nevra */

  GHashTable *fileoverride_pkgs; /* set of nevras */

  std::optional<rust::Box<rpmostreecxx::LockfileConfig>> lockfile;
  gboolean lockfile_strict;

  GLnxTmpDir tmpdir;

  gboolean kernel_changed;

  int tmprootfs_dfd; /* Borrowed */
  GHashTable *rootfs_usrlinks;
  GLnxTmpDir repo_tmpdir; /* Used to assemble+commit if no base rootfs provided */
};

G_END_DECLS
