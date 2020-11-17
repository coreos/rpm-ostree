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
#include <libdnf/libdnf.h>
#include "libglnx.h"
#include "rpmostree.h"
#include "rpmostree-types.h"

#define _N(single, plural, n) ( (n) == 1 ? (single) : (plural) )
#define _NS(n) _N("", "s", n)

#define VAR_SELINUX_TARGETED_PATH "var/lib/selinux/targeted/"

int
rpmostree_ptrarray_sort_compare_strings (gconstpointer ap,
                                         gconstpointer bp);

GVariant *_rpmostree_vardict_lookup_value_required (GVariantDict *dict,
                                                    const char *key,
                                                    const GVariantType *fmt,
                                                    GError     **error);

char *
_rpmostree_varsubst_string (const char *instr,
                            GHashTable *substitutions,
                            GError **error);

gboolean
_rpmostree_util_update_checksum_from_file (GChecksum    *checksum,
                                           int           rootfs_dfd,
                                           const char   *path,
                                           GCancellable *cancellable,
                                           GError      **error);

gboolean
rpmostree_pkg_is_local (DnfPackage *pkg);

char *
rpmostree_pkg_get_local_path (DnfPackage *pkg);

gboolean
rpmostree_check_size_within_limit (guint64     actual,
                                   guint64     limit,
                                   const char *subject,
                                   GError    **error);

char*
rpmostree_translate_path_for_ostree (const char *path);

char *
_rpmostree_util_next_version (const char   *auto_version_prefix,
                              const char   *version_suffix,
                              const char   *last_version,
                              GError      **error);

char *
rpmostree_str_replace (const char  *buf,
                       const char  *old,
                       const char  *new,
                       GError     **error);

char *
rpmostree_checksum_version (GVariant *checksum);

gboolean
rpmostree_pull_content_only (OstreeRepo  *dest,
                             OstreeRepo  *src,
                             const char  *src_commit,
                             GCancellable *cancellable,
                             GError      **error);
const char *
rpmostree_file_get_path_cached (GFile *file);

static inline
const char *
gs_file_get_path_cached (GFile *file)
{
  return rpmostree_file_get_path_cached (file);
}

gboolean rpmostree_stdout_is_journal (void);

char*
rpmostree_generate_diff_summary (guint upgraded,
                                 guint downgraded,
                                 guint removed,
                                 guint added);

typedef enum {
  RPMOSTREE_DIFF_PRINT_FORMAT_SUMMARY,       /* Diff: 1 upgraded, 2 downgraded, 3 removed, 4 added */
  RPMOSTREE_DIFF_PRINT_FORMAT_FULL_ALIGNED,  /* Upgraded: ...
                                                          ...
                                                   Added: ...
                                                          ...  */
  RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE /* Upgraded:
                                                   ...
                                                   ...
                                                 Added:
                                                   ...
                                                   ...  */
} RpmOstreeDiffPrintFormat;

void
rpmostree_diff_print_formatted (RpmOstreeDiffPrintFormat format,
                                const char *prefix,
                                guint      max_key_len,
                                GPtrArray *removed,
                                GPtrArray *added,
                                GPtrArray *modified_old,
                                GPtrArray *modified_new);

void
rpmostree_variant_diff_print_formatted (guint     max_key_len,
                                        GVariant *upgraded,
                                        GVariant *downgraded,
                                        GVariant *removed,
                                        GVariant *added);

void
rpmostree_diff_print (GPtrArray *removed,
                      GPtrArray *added,
                      GPtrArray *modified_old,
                      GPtrArray *modified_new);

gboolean
rpmostree_str_has_prefix_in_strv (const char *str,
                                  char      **strv,
                                  int         n);

gboolean
rpmostree_str_has_prefix_in_ptrarray (const char *str,
                                      GPtrArray  *prefixes);

gboolean
rpmostree_str_ptrarray_contains (GPtrArray  *strs,
                                 const char *str);

/* XXX: just return a struct out of these */
gboolean
rpmostree_deployment_get_layered_info (OstreeRepo        *repo,
                                       OstreeDeployment  *deployment,
                                       gboolean          *out_is_layered,
                                       guint             *out_layer_version,
                                       char             **out_base_layer,
                                       char            ***out_layered_pkgs,
                                       GVariant         **out_removed_base_pkgs,
                                       GVariant         **out_replaced_base_pkgs,
                                       GError           **error);

/* simpler version of the above */
gboolean
rpmostree_deployment_get_base_layer (OstreeRepo        *repo,
                                     OstreeDeployment  *deployment,
                                     char             **out_base_layer,
                                     GError           **error);

gboolean
rpmostree_migrate_pkgcache_repo (OstreeRepo   *repo,
                                 GCancellable *cancellable,
                                 GError      **error);

gboolean
rpmostree_decompose_sha256_nevra (const char **nevra,
                                  char       **sha256,
                                  GError     **error);

char*
rpmostree_get_deployment_root (OstreeSysroot     *sysroot,
                               OstreeDeployment *deployment);

char *
rpmostree_commit_content_checksum (GVariant *commit);

/* https://github.com/ostreedev/ostree/pull/1132 */
typedef struct {
  gboolean initialized;
  gboolean commit_on_failure;
  OstreeRepo *repo;
} RpmOstreeRepoAutoTransaction;

static inline void
rpmostree_repo_auto_transaction_cleanup (void *p)
{
  RpmOstreeRepoAutoTransaction *autotxn = p;
  if (!autotxn->initialized)
    return;

  /* If e.g. package downloads are cancelled, we still want to (try to) commit
   * to avoid redownloading.
   */
  if (autotxn->commit_on_failure)
    (void) ostree_repo_commit_transaction (autotxn->repo, NULL, NULL, NULL);
  else
    (void) ostree_repo_abort_transaction (autotxn->repo, NULL, NULL);
}

static inline gboolean
rpmostree_repo_auto_transaction_start (RpmOstreeRepoAutoTransaction     *autotxn,
                                       OstreeRepo                       *repo,
                                       gboolean                          commit_on_failure,
                                       GCancellable                     *cancellable,
                                       GError                          **error)
{
  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;
  autotxn->commit_on_failure = commit_on_failure;
  autotxn->repo = repo;
  autotxn->initialized = TRUE;
  return TRUE;
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (RpmOstreeRepoAutoTransaction, rpmostree_repo_auto_transaction_cleanup)

gboolean
rpmostree_variant_bsearch_str (GVariant   *array,
                               const char *str,
                               int        *out_pos);

const char*
rpmostree_auto_update_policy_to_str (RpmostreedAutomaticUpdatePolicy policy,
                                     GError **error);

gboolean
rpmostree_str_to_auto_update_policy (const char *str,
                                     RpmostreedAutomaticUpdatePolicy *out_policy,
                                     GError **error);

char*
rpmostree_timestamp_str_from_unix_utc (guint64 t);

gboolean
rpmostree_relative_path_is_ostree_compliant (const char *path);

char*
rpmostree_maybe_shell_quote (const char *s);
