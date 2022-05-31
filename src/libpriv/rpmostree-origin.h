/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

#include "rpmostree-core.h"
#include <ostree.h>

typedef struct RpmOstreeOrigin RpmOstreeOrigin;
RpmOstreeOrigin *rpmostree_origin_ref (RpmOstreeOrigin *origin);
void rpmostree_origin_unref (RpmOstreeOrigin *origin);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmOstreeOrigin, rpmostree_origin_unref)

RpmOstreeOrigin *rpmostree_origin_parse_keyfile (GKeyFile *keyfile, GError **error);

RpmOstreeOrigin *rpmostree_origin_parse_deployment (OstreeDeployment *deployment, GError **error);

RpmOstreeOrigin *rpmostree_origin_dup (RpmOstreeOrigin *origin);

rpmostreecxx::Refspec rpmostree_origin_get_refspec (RpmOstreeOrigin *origin);

rust::String rpmostree_origin_get_custom_url (RpmOstreeOrigin *origin);

rust::String rpmostree_origin_get_custom_description (RpmOstreeOrigin *origin);

rust::Vec<rust::String> rpmostree_origin_get_packages (RpmOstreeOrigin *origin);

bool rpmostree_origin_has_packages (RpmOstreeOrigin *origin);

bool rpmostree_origin_has_modules_enable (RpmOstreeOrigin *origin);

rust::Vec<rust::String> rpmostree_origin_get_local_packages (RpmOstreeOrigin *origin);

rust::Vec<rust::String> rpmostree_origin_get_local_fileoverride_packages (RpmOstreeOrigin *origin);

rust::Vec<rust::String> rpmostree_origin_get_overrides_remove (RpmOstreeOrigin *origin);

bool rpmostree_origin_has_overrides_remove_name (RpmOstreeOrigin *origin, const char *name);

rust::Vec<rust::String> rpmostree_origin_get_overrides_local_replace (RpmOstreeOrigin *origin);

rust::String rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin);

rust::Vec<rust::String> rpmostree_origin_get_initramfs_etc_files (RpmOstreeOrigin *origin);

bool rpmostree_origin_has_initramfs_etc_files (RpmOstreeOrigin *origin);

bool rpmostree_origin_get_regenerate_initramfs (RpmOstreeOrigin *origin);

rust::Vec<rust::String> rpmostree_origin_get_initramfs_args (RpmOstreeOrigin *origin);

rust::String rpmostree_origin_get_unconfigured_state (RpmOstreeOrigin *origin);

bool rpmostree_origin_may_require_local_assembly (RpmOstreeOrigin *origin);

bool rpmostree_origin_has_any_packages (RpmOstreeOrigin *origin);

GKeyFile *rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin);

bool rpmostree_origin_initramfs_etc_files_track (RpmOstreeOrigin *origin,
                                                 rust::Vec<rust::String> paths);

bool rpmostree_origin_initramfs_etc_files_untrack (RpmOstreeOrigin *origin,
                                                   rust::Vec<rust::String> paths);

bool rpmostree_origin_initramfs_etc_files_untrack_all (RpmOstreeOrigin *origin);

void rpmostree_origin_set_regenerate_initramfs (RpmOstreeOrigin *origin, gboolean regenerate,
                                                rust::Vec<rust::String> args);

void rpmostree_origin_set_override_commit (RpmOstreeOrigin *origin, const char *checksum);

bool rpmostree_origin_get_cliwrap (RpmOstreeOrigin *origin);
void rpmostree_origin_set_cliwrap (RpmOstreeOrigin *origin, bool cliwrap);

void rpmostree_origin_set_rebase (RpmOstreeOrigin *origin, const char *new_refspec);
void rpmostree_origin_set_rebase_custom (RpmOstreeOrigin *origin, const char *new_refspec,
                                         const char *custom_origin_url,
                                         const char *custom_origin_description);

gboolean rpmostree_origin_add_packages (RpmOstreeOrigin *origin, rust::Vec<rust::String> packages,
                                        gboolean allow_existing, gboolean *out_changed,
                                        GError **error);

gboolean rpmostree_origin_add_local_packages (RpmOstreeOrigin *origin,
                                              rust::Vec<rust::String> packages,
                                              gboolean allow_existing, gboolean *out_changed,
                                              GError **error);

gboolean rpmostree_origin_add_local_fileoverride_packages (RpmOstreeOrigin *origin,
                                                           rust::Vec<rust::String> packages,
                                                           gboolean allow_existing,
                                                           gboolean *out_changed, GError **error);

gboolean rpmostree_origin_remove_packages (RpmOstreeOrigin *origin,
                                           rust::Vec<rust::String> packages, gboolean allow_noent,
                                           gboolean *out_changed, GError **error);
gboolean rpmostree_origin_remove_all_packages (RpmOstreeOrigin *origin);

gboolean rpmostree_origin_add_modules (RpmOstreeOrigin *origin, rust::Vec<rust::String> modules,
                                       gboolean enable_only);

gboolean rpmostree_origin_remove_modules (RpmOstreeOrigin *origin, rust::Vec<rust::String> modules,
                                          gboolean enable_only);

gboolean rpmostree_origin_add_override_remove (RpmOstreeOrigin *origin,
                                               rust::Vec<rust::String> packages, GError **error);
gboolean rpmostree_origin_add_override_replace_local (RpmOstreeOrigin *origin,
                                                      rust::Vec<rust::String> packages,
                                                      GError **error);

gboolean rpmostree_origin_remove_override_remove (RpmOstreeOrigin *origin, const char *package);

gboolean rpmostree_origin_remove_override_replace_local (RpmOstreeOrigin *origin,
                                                         const char *package);

gboolean rpmostree_origin_remove_override_replace (RpmOstreeOrigin *origin, const char *package);

gboolean rpmostree_origin_remove_all_overrides (RpmOstreeOrigin *origin);
