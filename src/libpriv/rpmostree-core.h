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

#define RPMOSTREE_TYPE_CONTEXT (rpmostree_context_get_type ())
G_DECLARE_FINAL_TYPE (RpmOstreeContext, rpmostree_context, RPMOSTREE, CONTEXT, GObject)

#define RPMOSTREE_TYPE_TREESPEC (rpmostree_treespec_get_type ())
G_DECLARE_FINAL_TYPE (RpmOstreeTreespec, rpmostree_treespec, RPMOSTREE, TREESPEC, GObject)

#define RPMOSTREE_TYPE_INSTALL (rpmostree_install_get_type ())
G_DECLARE_FINAL_TYPE (RpmOstreeInstall, rpmostree_install, RPMOSTREE, INSTALL, GObject)

RpmOstreeContext *rpmostree_context_new_system (GCancellable *cancellable,
                                                GError **error);

RpmOstreeContext *rpmostree_context_new_unprivileged (int basedir_dfd,
                                                      GCancellable *cancellable,
                                                      GError **error);

HifContext * rpmostree_context_get_hif (RpmOstreeContext *self);

RpmOstreeTreespec *rpmostree_treespec_new_from_keyfile (GKeyFile *keyfile, GError  **error);
RpmOstreeTreespec *rpmostree_treespec_new_from_path (const char *path, GError  **error);
RpmOstreeTreespec *rpmostree_treespec_new (GVariant   *variant);

GHashTable *rpmostree_context_get_varsubsts (RpmOstreeContext *context);

GVariant *rpmostree_treespec_to_variant (RpmOstreeTreespec *spec);
const char *rpmostree_treespec_get_ref (RpmOstreeTreespec *spec);

gboolean rpmostree_context_setup (RpmOstreeContext     *self,
                                  const char    *install_root,
                                  RpmOstreeTreespec *treespec,
                                  GCancellable  *cancellable,
                                  GError       **error);

void rpmostree_context_set_repo (RpmOstreeContext *self,
                                 OstreeRepo *repo);

void rpmostree_hif_add_checksum_goal (GChecksum *checksum, HyGoal goal);
char *rpmostree_context_get_state_sha512 (RpmOstreeContext *self);

char *rpmostree_get_cache_branch_header (Header hdr);
char *rpmostree_get_cache_branch_pkg (HifPackage *pkg);

gboolean rpmostree_context_download_metadata (RpmOstreeContext  *context,
                                               GCancellable      *cancellable,
                                               GError           **error);

/* This API allocates an install context, use with one of the later ones */
gboolean rpmostree_context_prepare_install (RpmOstreeContext     *self,
                                            RpmOstreeInstall **out_install,
                                            GCancellable   *cancellable,
                                            GError        **error);

gboolean rpmostree_context_download_rpms (RpmOstreeContext     *self,
                                          int             target_dfd,
                                          RpmOstreeInstall *install,
                                          GCancellable   *cancellable,
                                          GError        **error);

gboolean rpmostree_context_download_import (RpmOstreeContext     *self,
                                          RpmOstreeInstall *install,
                                          GCancellable               *cancellable,
                                          GError                    **error);

gboolean rpmostree_context_assemble_commit (RpmOstreeContext     *self,
                                            int                   tmpdir_dfd,
                                            const char           *name,
                                            char                **out_commit,
                                            GCancellable         *cancellable,
                                            GError              **error);
