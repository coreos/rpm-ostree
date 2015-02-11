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
#include <ostree.h>

void
_rpmostree_set_error_from_errno (GError    **error,
                                 gint        errsv);

void
_rpmostree_set_prefix_error_from_errno (GError     **error,
                                        gint         errsv,
                                        const char  *format,
                                        ...) G_GNUC_PRINTF (3,4);

void _rpmostree_perror_fatal (const char *message) __attribute__ ((noreturn));

GVariant *_rpmostree_vardict_lookup_value_required (GVariantDict *dict,
                                                    const char *key,
                                                    const GVariantType *fmt,
                                                    GError     **error);

gboolean rpmostree_mkdtemp (const char   *template,
                             char       **out_tmpdir,
                             int         *out_tmpdir_dfd,  /* allow-none */
                             GError     **error);

char *
_rpmostree_varsubst_string (const char *instr,
                            GHashTable *substitutions,
                            GError **error);

gboolean
_rpmostree_util_enumerate_directory_allow_noent (GFile               *dirpath,
						 const char          *queryargs,
						 GFileQueryInfoFlags  queryflags,
						 GFileEnumerator    **out_direnum,
						 GCancellable        *cancellable,
						 GError             **error);

gboolean
_rpmostree_file_load_contents_utf8_allow_noent (GFile          *path,
                                                char          **out_contents,
                                                GCancellable   *cancellable,
                                                GError        **error);

gboolean
_rpmostree_util_update_checksum_from_file (GChecksum    *checksum,
                                           GFile        *src,
                                           GCancellable *cancellable,
                                           GError      **error);

GPtrArray *
_rpmostree_util_get_commit_hashes (OstreeRepo *repo,
                                   const char *beg,
                                   const char *end,
                                   GCancellable *cancellable,
                                   GError **error);

gboolean
_rpmostree_sync_wait_on_pid (pid_t          pid,
                             GError       **error);

char *
_rpmostree_util_next_version (const char *auto_version_prefix,
                              const char *last_version);

GKeyFile *
_rpmostree_util_keyfile_clone (GKeyFile *keyfile);

gboolean
_rpmostree_util_parse_origin (GKeyFile         *origin,
                              char            **out_refspec,
                              char           ***out_packages,
                              GError          **error);
