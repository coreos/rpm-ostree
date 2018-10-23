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

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <gio/gunixoutputstream.h>
#include <systemd/sd-journal.h>

#include "rpmostree-util.h"
#include "rpmostree-rust.h"
#include "rpmostree-origin.h"
#include "rpmostree-output.h"
#include "libsd-locale-util.h"
#include "libglnx.h"

#define RPMOSTREE_OLD_PKGCACHE_DIR "extensions/rpmostree/pkgcache"

int
rpmostree_ptrarray_sort_compare_strings (gconstpointer ap,
                                         gconstpointer bp)
{
  char **asp = (gpointer)ap;
  char **bsp = (gpointer)bp;
  return strcmp (*asp, *bsp);
}

GVariant *
_rpmostree_vardict_lookup_value_required (GVariantDict *dict,
                                          const char *key,
                                          const GVariantType *fmt,
                                          GError     **error)
{
  GVariant *r = g_variant_dict_lookup_value (dict, key, fmt);
  if (!r)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Failed to find metadata key %s (signature %s)", key, (char*)fmt);
      return NULL;
    }
  return r;
}

/* Given a string of the form
 * "bla blah ${foo} blah ${bar}"
 * and a hash table of variables, substitute the variable values.
 */
char *
_rpmostree_varsubst_string (const char *instr,
                            GHashTable *substitutions,
                            GError **error)
{
  return ror_util_varsubst (instr, substitutions, error);
}

gboolean
_rpmostree_util_update_checksum_from_file (GChecksum    *checksum,
                                           int           dfd,
                                           const char   *path,
                                           GCancellable *cancellable,
                                           GError      **error)
{
  glnx_autofd int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;

  if (!glnx_openat_rdonly (dfd, path, TRUE, &fd, error))
    return FALSE;

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return FALSE;

  g_checksum_update (checksum, (guint8*)g_mapped_file_get_contents (mfile),
                     g_mapped_file_get_length (mfile));

  return TRUE;
}

static char *
ost_get_prev_commit (OstreeRepo *repo, char *checksum)
{
  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &commit, NULL))
    return NULL;

  return ostree_commit_get_parent (commit);
}

GPtrArray *
_rpmostree_util_get_commit_hashes (OstreeRepo    *repo,
                                   const char    *beg,
                                   const char    *end,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  GPtrArray *ret = NULL;
  g_autofree char *beg_checksum = NULL;
  g_autofree char *end_checksum = NULL;
  g_autofree char *parent = NULL;
  char *checksum = NULL;
  gboolean worked = FALSE;

  if (!ostree_repo_read_commit (repo, beg, NULL, &beg_checksum, cancellable, error))
    goto out;

  ret = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (ret, g_strdup (beg));  /* Add the user defined REFSPEC. */

  if (end &&
      !ostree_repo_read_commit (repo, end, NULL, &end_checksum, cancellable, error))
      goto out;

  if (end && g_str_equal (end_checksum, beg_checksum))
      goto worked_out;

  checksum = beg_checksum;
  while ((parent = ost_get_prev_commit (repo, checksum)))
    {
      if (end && g_str_equal (end_checksum, parent))
        { /* Add the user defined REFSPEC. */
          g_ptr_array_add (ret, g_strdup (end));
          break;
        }

      g_ptr_array_add (ret, parent);
      checksum = parent;
    }

  if (end && !parent)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid ref range: %s is not a parent of %s", end, beg);
      goto out;
    }

 worked_out:
  worked = TRUE;

 out:
  if (!worked)
    {
      g_ptr_array_free (ret, TRUE);
      ret = NULL;
    }
  return ret;
}

/* Returns TRUE if a package is originally on a locally-accessible filesystem */
gboolean
rpmostree_pkg_is_local (DnfPackage *pkg)
{
  const char *reponame = dnf_package_get_reponame (pkg);
  return (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) == 0 ||
          dnf_repo_is_local (dnf_package_get_repo (pkg)));
}

/* Returns the local filesystem path for a package; for non-local packages,
 * it must have already been downloaded.
 */
char *
rpmostree_pkg_get_local_path (DnfPackage *pkg)
{
  DnfRepo *pkg_repo = dnf_package_get_repo (pkg);
  const gboolean is_local = rpmostree_pkg_is_local (pkg);
  if (is_local)
    return g_strdup (dnf_package_get_filename (pkg));
  else
    {
      const char *pkg_location = dnf_package_get_location (pkg);
      return g_build_filename (dnf_repo_get_location (pkg_repo),
                               "packages", glnx_basename (pkg_location), NULL);
    }
}

gboolean
rpmostree_check_size_within_limit (guint64     actual,
                                   guint64     limit,
                                   const char *subject,
                                   GError    **error)
{
  if (actual <= limit)
    return TRUE;
  g_autofree char *max_formatted = g_format_size (limit);
  g_autofree char *found_formatted = g_format_size (actual);
  return glnx_throw (error, "Exceeded maximum size %s; %s is of size: %s",
                     max_formatted, subject, found_formatted);
}

/* Convert a "traditional" path (normally from e.g. an RPM) into its final location in
 * ostree */
char*
rpmostree_translate_path_for_ostree (const char *path)
{
  if (g_str_has_prefix (path, "etc/"))
    return g_strconcat ("usr/", path, NULL);
  else if (g_str_has_prefix (path, "boot/"))
    return g_strconcat ("usr/lib/ostree-boot/", path + strlen ("boot/"), NULL);
  /* Special hack for https://bugzilla.redhat.com/show_bug.cgi?id=1290659
   * See also commit 4a86bdd19665700fa308461510c9decd63e31a03
   * and rpmostree_postprocess_selinux_policy_store_location().
   */
  else if (g_str_has_prefix (path, VAR_SELINUX_TARGETED_PATH))
    return g_strconcat ("usr/etc/selinux/targeted/", path + strlen (VAR_SELINUX_TARGETED_PATH), NULL);

  return NULL;
}

char *
_rpmostree_util_next_version (const char *auto_version_prefix,
                              const char *last_version)
{
  unsigned long long num = 0;
  const char *end = NULL;

  if (!last_version || !g_str_has_prefix (last_version, auto_version_prefix))
    return g_strdup (auto_version_prefix);

  if (g_str_equal (last_version, auto_version_prefix))
    return g_strdup_printf ("%s.1", auto_version_prefix);

  end = last_version + strlen(auto_version_prefix);

  if (*end != '.')
    return g_strdup (auto_version_prefix);
  ++end;

  num = g_ascii_strtoull (end, NULL, 10);
  return g_strdup_printf ("%s.%llu", auto_version_prefix, num + 1);
}

/* Replace every occurrence of @old in @buf with @new. */
char *
rpmostree_str_replace (const char  *buf,
                       const char  *old,
                       const char  *new,
                       GError     **error)
{
  g_autofree char *literal_old = g_regex_escape_string (old, -1);
  g_autoptr(GRegex) regex = g_regex_new (literal_old, 0, 0, error);

  if (regex == NULL)
    return NULL;

  return g_regex_replace_literal (regex, buf, -1, 0, new, 0, error);
}

static gboolean
pull_content_only_recurse (OstreeRepo  *dest,
                           OstreeRepo  *src,
                           OstreeRepoCommitTraverseIter *iter,
                           GCancellable *cancellable,
                           GError      **error)
{
  gboolean done = FALSE;

  while (!done)
    {
      OstreeRepoCommitIterResult iterres =
        ostree_repo_commit_traverse_iter_next (iter, cancellable, error);

      switch (iterres)
        {
        case OSTREE_REPO_COMMIT_ITER_RESULT_ERROR:
          return FALSE;
        case OSTREE_REPO_COMMIT_ITER_RESULT_END:
          done = TRUE;
          break;
        case OSTREE_REPO_COMMIT_ITER_RESULT_FILE:
          {
            char *name;
            char *checksum;

            ostree_repo_commit_traverse_iter_get_file (iter, &name, &checksum);

            if (!ostree_repo_import_object_from (dest, src, OSTREE_OBJECT_TYPE_FILE,
                                                 checksum, cancellable, error))
              return FALSE;
          }
          break;
        case OSTREE_REPO_COMMIT_ITER_RESULT_DIR:
          {
            char *name;
            char *content_checksum;
            char *meta_checksum;
            g_autoptr(GVariant) dirtree = NULL;
            ostree_cleanup_repo_commit_traverse_iter
              OstreeRepoCommitTraverseIter subiter = { 0, };

            ostree_repo_commit_traverse_iter_get_dir (iter, &name, &content_checksum, &meta_checksum);

            if (!ostree_repo_load_variant (src, OSTREE_OBJECT_TYPE_DIR_TREE,
                                           content_checksum, &dirtree,
                                           error))
              return FALSE;

            if (!ostree_repo_commit_traverse_iter_init_dirtree (&subiter, src, dirtree,
                                                                OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                                error))
              return FALSE;

            if (!pull_content_only_recurse (dest, src, &subiter, cancellable, error))
              return FALSE;
          }
          break;
        }
    }

  return TRUE;
}

/* Migrate only the content (.file) objects from src+src_commit into dest.
 * Used for package layering.
 */
gboolean
rpmostree_pull_content_only (OstreeRepo  *dest,
                             OstreeRepo  *src,
                             const char  *src_commit,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GVariant) commitdata = NULL;
  ostree_cleanup_repo_commit_traverse_iter
    OstreeRepoCommitTraverseIter iter = { 0, };

  if (!ostree_repo_load_commit (src, src_commit, &commitdata, NULL, error))
    return FALSE;

  if (!ostree_repo_commit_traverse_iter_init_commit (&iter, src, commitdata,
                                                     OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                     error))
    return FALSE;

  if (!pull_content_only_recurse (dest, src, &iter, cancellable, error))
    return FALSE;

  return TRUE;
}

G_LOCK_DEFINE_STATIC (pathname_cache);

/**
 * rpmostree_file_get_path_cached:
 *
 * Like g_file_get_path(), but returns a constant copy so callers
 * don't need to free the result.
 */
const char *
rpmostree_file_get_path_cached (GFile *file)
{
  const char *path;
  static GQuark _file_path_quark = 0;

  if (G_UNLIKELY (_file_path_quark) == 0)
    _file_path_quark = g_quark_from_static_string ("gsystem-file-path");

  G_LOCK (pathname_cache);

  path = g_object_get_qdata ((GObject*)file, _file_path_quark);
  if (!path)
    {
      path = g_file_get_path (file);
      if (path == NULL)
        {
          G_UNLOCK (pathname_cache);
          return NULL;
        }
      g_object_set_qdata_full ((GObject*)file, _file_path_quark, (char*)path, (GDestroyNotify)g_free);
    }

  G_UNLOCK (pathname_cache);

  return path;
}

gboolean
rpmostree_str_has_prefix_in_strv (const char *str,
                                  char      **prefixes,
                                  int         n)
{
  if (n < 0)
    n = g_strv_length (prefixes);
  for (int i = 0; i < n; i++)
    {
      if (g_str_has_prefix (str, prefixes[i]))
        return TRUE;
    }
  return FALSE;
}

gboolean
rpmostree_str_has_prefix_in_ptrarray (const char *str,
                                      GPtrArray  *prefixes)
{
  return rpmostree_str_has_prefix_in_strv (str, (char**)prefixes->pdata, prefixes->len);
}

/**
 * rpmostree_str_ptrarray_contains:
 *
 * Like g_strv_contains() but for ptrarray.
 * Handles strs==NULL as an empty array.
 */
gboolean
rpmostree_str_ptrarray_contains (GPtrArray  *strs,
                                 const char *str)
{
  g_assert (str);
  if (!strs)
    return FALSE;

  guint n = strs->len;
  for (guint i = 0; i < n; i++)
    {
      if (g_str_equal (str, strs->pdata[i]))
        return TRUE;
    }
  return FALSE;
}

gboolean
rpmostree_deployment_get_layered_info (OstreeRepo        *repo,
                                       OstreeDeployment  *deployment,
                                       gboolean          *out_is_layered,
                                       guint             *out_layer_version,
                                       char             **out_base_layer,
                                       char            ***out_layered_pkgs,
                                       GVariant         **out_removed_base_pkgs,
                                       GVariant         **out_replaced_base_pkgs,
                                       GError           **error)
{
  const char *csum = ostree_deployment_get_csum (deployment);
  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, csum, &commit, NULL, error))
    return FALSE;

  g_autoptr(GVariant) metadata = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) dict = g_variant_dict_new (metadata);

  /* More recent versions have an explicit clientlayer attribute (which
   * realistically will always be TRUE). For older versions, we just
   * rely on the treespec being present. */
  gboolean is_layered = FALSE;
  if (!g_variant_dict_lookup (dict, "rpmostree.clientlayer", "b", &is_layered))
    is_layered = g_variant_dict_contains (dict, "rpmostree.spec");

  guint clientlayer_version = 0;
  g_variant_dict_lookup (dict, "rpmostree.clientlayer_version", "u",
                         &clientlayer_version);

  /* only fetch base if we have to */
  g_autofree char *base_layer = NULL;
  if (is_layered && out_base_layer != NULL)
    {
      base_layer = ostree_commit_get_parent (commit);
      g_assert (base_layer);
    }

  /* only fetch pkgs if we have to */
  g_auto(GStrv) layered_pkgs = NULL;
  g_autoptr(GVariant) removed_base_pkgs = NULL;
  g_autoptr(GVariant) replaced_base_pkgs = NULL;
  if (is_layered && (out_layered_pkgs != NULL || out_removed_base_pkgs != NULL))
    {
      /* starting from v1, we no longer embed a treespec in client layers */
      if (clientlayer_version >= 1)
        {
          g_assert (g_variant_dict_lookup (dict, "rpmostree.packages", "^as",
                                           &layered_pkgs));
        }
      else
        {
          g_autoptr(GVariant) treespec_v = NULL;
          g_autoptr(GVariantDict) treespec = NULL;

          g_assert (g_variant_dict_contains (dict, "rpmostree.spec"));

           /* there should always be a treespec */
          treespec_v = g_variant_dict_lookup_value (dict, "rpmostree.spec",
                                                    G_VARIANT_TYPE ("a{sv}"));
          g_assert (treespec_v);

          /* there should always be a packages entry, even if empty */
          treespec = g_variant_dict_new (treespec_v);
          g_assert (g_variant_dict_lookup (treespec, "packages", "^as",
                                           &layered_pkgs));
        }

      if (clientlayer_version >= 2)
        {
          removed_base_pkgs =
            g_variant_dict_lookup_value (dict, "rpmostree.removed-base-packages",
                                         G_VARIANT_TYPE ("av"));
          g_assert (removed_base_pkgs);

          replaced_base_pkgs =
            g_variant_dict_lookup_value (dict, "rpmostree.replaced-base-packages",
                                         G_VARIANT_TYPE ("a(vv)"));
          g_assert (replaced_base_pkgs);
        }
    }

  /* canonicalize outputs to empty array */

  if (out_is_layered != NULL)
    *out_is_layered = is_layered;
  if (out_layer_version != NULL)
    *out_layer_version = clientlayer_version;
  if (out_base_layer != NULL)
    *out_base_layer = g_steal_pointer (&base_layer);
  if (out_layered_pkgs != NULL)
    {
      if (!layered_pkgs)
        layered_pkgs = g_new0 (char*, 1);
      *out_layered_pkgs = g_steal_pointer (&layered_pkgs);
    }
  if (out_removed_base_pkgs != NULL)
    {
      if (!removed_base_pkgs)
        removed_base_pkgs =
          g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("v"), NULL, 0));
      *out_removed_base_pkgs = g_steal_pointer (&removed_base_pkgs);
    }
  if (out_replaced_base_pkgs != NULL)
    {
      if (!replaced_base_pkgs)
        replaced_base_pkgs =
          g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(vv)"), NULL, 0));
      *out_replaced_base_pkgs = g_steal_pointer (&replaced_base_pkgs);
    }

  return TRUE;
}

/* Returns the base layer checksum if layered, NULL otherwise. */
gboolean
rpmostree_deployment_get_base_layer (OstreeRepo        *repo,
                                     OstreeDeployment  *deployment,
                                     char             **out_base_layer,
                                     GError           **error)
{
  return rpmostree_deployment_get_layered_info (repo, deployment, NULL, NULL,
                                                out_base_layer, NULL, NULL, NULL, error);
}

static gboolean
do_pkgcache_migration (OstreeRepo    *repo,
                       OstreeRepo    *pkgcache,
                       guint         *out_n_migrated,
                       GCancellable  *cancellable,
                       GError       **error)
{
  g_autoptr(GHashTable) pkgcache_refs = NULL;
  if (!ostree_repo_list_refs_ext (pkgcache, "rpmostree/pkg", &pkgcache_refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable, error))
    return FALSE;

  if (g_hash_table_size (pkgcache_refs) == 0)
    {
      *out_n_migrated = 0;
      return TRUE; /* Note early return */
    }

  g_autofree char **refs_strv =
    (char **)g_hash_table_get_keys_as_array (pkgcache_refs, NULL);

  /* fd-relative pull API anyone? build the path manually at least to avoid one malloc */
  g_autofree char *pkgcache_uri =
    g_strdup_printf ("file:///proc/self/fd/%d", ostree_repo_get_dfd (pkgcache));
  if (!ostree_repo_pull (repo, pkgcache_uri, refs_strv, OSTREE_REPO_PULL_FLAGS_NONE, NULL,
                         cancellable, error))
    return FALSE;

  *out_n_migrated = g_strv_length (refs_strv);
  return TRUE;
}

gboolean
rpmostree_migrate_pkgcache_repo (OstreeRepo   *repo,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);

  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (repo_dfd, RPMOSTREE_OLD_PKGCACHE_DIR, &stbuf,
                                 AT_SYMLINK_NOFOLLOW, error))
    return FALSE;

  if (errno == 0 && S_ISLNK (stbuf.st_mode))
    return TRUE;

  /* if pkgcache exists, we expect it to be valid; we don't want to nuke it just because of
   * a transient error */
  if (errno == 0)
    {
      if (S_ISDIR (stbuf.st_mode))
        {
          g_auto(RpmOstreeProgress) task = { 0, };
          rpmostree_output_task_begin (&task, "Migrating pkgcache");

          g_autoptr(OstreeRepo) pkgcache = ostree_repo_open_at (repo_dfd,
                                                                RPMOSTREE_OLD_PKGCACHE_DIR,
                                                                cancellable, error);
          if (!pkgcache)
            return FALSE;

          guint n_migrated;
          if (!do_pkgcache_migration (repo, pkgcache, &n_migrated, cancellable, error))
            return FALSE;

          rpmostree_output_progress_end_msg (&task, "%u done", n_migrated);
          if (n_migrated > 0)
            sd_journal_print (LOG_INFO, "migrated %u cached package%s to system repo",
                              n_migrated, _NS(n_migrated));
        }

      if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_OLD_PKGCACHE_DIR, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!glnx_shutil_mkdir_p_at (repo_dfd, dirname (strdupa (RPMOSTREE_OLD_PKGCACHE_DIR)),
                                   0755, cancellable, error))
        return FALSE;
    }

  /* leave a symlink for compatibility with older rpm-ostree versions */
  if (symlinkat ("../..", repo_dfd, RPMOSTREE_OLD_PKGCACHE_DIR) < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  return TRUE;
}

char*
rpmostree_get_deployment_root (OstreeSysroot     *sysroot,
                               OstreeDeployment *deployment)
{
  const char *sysroot_path = gs_file_get_path_cached (ostree_sysroot_get_path (sysroot));
  g_autofree char *deployment_dirpath =
    ostree_sysroot_get_deployment_dirpath (sysroot, deployment);
  return g_build_filename (sysroot_path, deployment_dirpath, NULL);
}

gboolean
rpmostree_decompose_sha256_nevra (const char **nevra, /* gets incremented */
                                  char       **out_sha256,
                                  GError     **error)
{
  const char *sha256_nevra = *nevra;
  g_autofree char *sha256 = NULL;

  if (strlen (sha256_nevra) < 66 || /* 64 + ":" + at least 1 char for nevra */
      sha256_nevra[64] != ':')
    return FALSE;

  sha256 = g_strndup (sha256_nevra, 64);
  if (!ostree_validate_checksum_string (sha256, error))
    return FALSE;

  *nevra += 65;
  if (out_sha256)
    *out_sha256 = g_steal_pointer (&sha256);

  return TRUE;
}

/* Comptute SHA-256(dirtree checksum, dirmeta checksum) of a commit;
 * this identifies the checksum of its *contents*, independent of metadata
 * like the commit timestamp.  https://github.com/ostreedev/ostree/issues/1315
 */
char *
rpmostree_commit_content_checksum (GVariant *commit)
{
  g_autoptr(GChecksum) hasher = g_checksum_new (G_CHECKSUM_SHA256);
  char checksum[OSTREE_SHA256_STRING_LEN+1];
  const guint8 *csum;

  /* Hash content checksum */
  g_autoptr(GVariant) csum_bytes = NULL;
  g_variant_get_child (commit, 6, "@ay", &csum_bytes);
  csum = ostree_checksum_bytes_peek (csum_bytes);
  ostree_checksum_inplace_from_bytes (csum, checksum);
  g_checksum_update (hasher, (guint8*)checksum, OSTREE_SHA256_STRING_LEN);
  g_clear_pointer (&csum_bytes, (GDestroyNotify)g_variant_unref);

  /* Hash meta checksum */
  g_variant_get_child (commit, 7, "@ay", &csum_bytes);
  csum = ostree_checksum_bytes_peek (csum_bytes);
  ostree_checksum_inplace_from_bytes (csum, checksum);
  g_checksum_update (hasher, (guint8*)checksum, OSTREE_SHA256_STRING_LEN);

  return g_strdup (g_checksum_get_string (hasher));
}

/* Implementation taken from https://git.gnome.org/browse/libgsystem/tree/src/gsystem-log.c */
gboolean
rpmostree_stdout_is_journal (void)
{
  static gsize initialized;
  static gboolean stdout_is_socket;

  if (g_once_init_enter (&initialized))
    {
      guint64 pid = (guint64) getpid ();
      g_autofree char *fdpath = g_strdup_printf ("/proc/%" G_GUINT64_FORMAT "/fd/1", pid);
      char buf[1024];
      ssize_t bytes_read;

      if ((bytes_read = readlink (fdpath, buf, sizeof(buf) - 1)) != -1)
        {
          buf[bytes_read] = '\0';
          stdout_is_socket = g_str_has_prefix (buf, "socket:");
        }
      else
        stdout_is_socket = FALSE;

      g_once_init_leave (&initialized, TRUE);
    }

  return stdout_is_socket;
}

/* Given the result of rpm_ostree_db_diff(), print it in a nice formatted way for humans. */
void
rpmostree_diff_print_formatted (GPtrArray *removed,
                                GPtrArray *added,
                                GPtrArray *modified_old,
                                GPtrArray *modified_new)
{
  gboolean first;

  g_assert_cmpuint (modified_old->len, ==, modified_new->len);

  first = TRUE;
  for (guint i = 0; i < modified_old->len; i++)
    {
      RpmOstreePackage *oldpkg = modified_old->pdata[i];
      RpmOstreePackage *newpkg = modified_new->pdata[i];
      const char *name = rpm_ostree_package_get_name (oldpkg);

      if (rpm_ostree_package_cmp (oldpkg, newpkg) > 0)
        continue;

      if (first)
        {
          g_print ("Upgraded:\n");
          first = FALSE;
        }

      g_print ("  %s %s -> %s\n", name,
               rpm_ostree_package_get_evr (oldpkg),
               rpm_ostree_package_get_evr (newpkg));
    }

  first = TRUE;
  for (guint i = 0; i < modified_old->len; i++)
    {
      RpmOstreePackage *oldpkg = modified_old->pdata[i];
      RpmOstreePackage *newpkg = modified_new->pdata[i];
      const char *name = rpm_ostree_package_get_name (oldpkg);

      if (rpm_ostree_package_cmp (oldpkg, newpkg) < 0)
        continue;

      if (first)
        {
          g_print ("Downgraded:\n");
          first = FALSE;
        }

      g_print ("  %s %s -> %s\n", name,
               rpm_ostree_package_get_evr (oldpkg),
               rpm_ostree_package_get_evr (newpkg));
    }

  if (removed->len > 0)
    g_print ("Removed:\n");
  for (guint i = 0; i < removed->len; i++)
    {
      RpmOstreePackage *pkg = removed->pdata[i];
      const char *nevra = rpm_ostree_package_get_nevra (pkg);

      g_print ("  %s\n", nevra);
    }

  if (added->len > 0)
    g_print ("Added:\n");
  for (guint i = 0; i < added->len; i++)
    {
      RpmOstreePackage *pkg = added->pdata[i];
      const char *nevra = rpm_ostree_package_get_nevra (pkg);

      g_print ("  %s\n", nevra);
    }
}

static void
variant_diff_print_modified (guint       max_key_len,
                             GVariant   *modified,
                             const char *type)
{
  guint n = g_variant_n_children (modified);
  for (guint i = 0; i < n; i++)
    {
      const char *name, *evr_old, *evr_new;
      g_variant_get_child (modified, i, "(u&s(&ss)(&ss))",
                           NULL, &name, &evr_old, NULL, &evr_new, NULL);
      g_print ("  %*s%s %s %s -> %s\n", max_key_len, i == 0 ? type : "", i == 0 ? ":" : " ",
               name, evr_old, evr_new);
    }
}

static void
variant_diff_print_singles (guint       max_key_len,
                            GVariant   *singles,
                            const char *type)
{
  guint n = g_variant_n_children (singles);
  for (guint i = 0; i < n; i++)
    {
      const char *name, *evr, *arch;
      g_variant_get_child (singles, i, "(u&s&s&s)", NULL, &name, &evr, &arch);
      g_print ("  %*s%s %s-%s.%s\n", max_key_len, i == 0 ? type : "", i == 0 ? ":" : " ",
               name, evr, arch);
    }
}

void
rpmostree_variant_diff_print_formatted (guint     max_key_len,
                                        GVariant *upgraded,
                                        GVariant *downgraded,
                                        GVariant *removed,
                                        GVariant *added)
{
  variant_diff_print_modified (max_key_len, upgraded, "Upgraded");
  variant_diff_print_modified (max_key_len, downgraded, "Downgraded");
  variant_diff_print_singles (max_key_len, removed, "Removed");
  variant_diff_print_singles (max_key_len, added, "Added");
}

static int
pkg_cmp_end (RpmOstreePackage *a, RpmOstreePackage *b)
{
  if (!b)
    return -1;
  if (!a)
    return +1;

  return rpm_ostree_package_cmp (a, b);
}

/* Given the result of rpm_ostree_db_diff(), print it in diff format for scripts. */
void
rpmostree_diff_print (GPtrArray *removed,
                      GPtrArray *added,
                      GPtrArray *modified_old,
                      GPtrArray *modified_new)
{
  g_assert_cmpuint (modified_old->len, ==, modified_new->len);

  const guint an = added->len;
  const guint rn = removed->len;
  const guint mn = modified_old->len;

  guint cur_a = 0;
  guint cur_r = 0;
  guint cur_m = 0;
  while (cur_a < an || cur_r < rn || cur_m < mn)
    {
      RpmOstreePackage *pkg_a = cur_a < an ? added->pdata[cur_a] : NULL;
      RpmOstreePackage *pkg_r = cur_r < rn ? removed->pdata[cur_r] : NULL;
      RpmOstreePackage *pkg_m = cur_m < mn ? modified_old->pdata[cur_m] : NULL;

      if (pkg_cmp_end (pkg_m, pkg_r) < 0)
        if (pkg_cmp_end (pkg_m, pkg_a) < 0)
          { /* mod is first */
            g_print ("!%s\n", rpm_ostree_package_get_nevra (pkg_m));
            g_print ("=%s\n", rpm_ostree_package_get_nevra (modified_new->pdata[cur_m]));
            cur_m++;
          }
        else
          { /* add is first */
            g_print ("+%s\n", rpm_ostree_package_get_nevra (pkg_a));
            cur_a++;
          }
      else
        if (pkg_cmp_end (pkg_r, pkg_a) < 0)
          { /* del is first */
            g_print ("-%s\n", rpm_ostree_package_get_nevra (pkg_r));
            cur_r++;
          }
        else
          { /* add is first */
            g_print ("+%s\n", rpm_ostree_package_get_nevra (pkg_a));
            cur_a++;
          }
    }
}

/* Copy of ot_variant_bsearch_str() from libostree
 * @array: A GVariant array whose first element must be a string
 * @str: Search for this string
 * @out_pos: Output position
 *
 * Binary search in a GVariant array, which must be of the form 'a(s...)', where
 * '...' may be anything.  The array elements must be sorted. If duplicates are
 * present, the earliest match will be returned.
 *
 * Returns: %TRUE iff found
 */
gboolean
rpmostree_variant_bsearch_str (GVariant   *array,
                               const char *str,
                               int        *out_pos)
{
  const gsize n = g_variant_n_children (array);
  if (n == 0)
    return FALSE;

  gsize imax = n - 1;
  gsize imin = 0;
  gsize imid = -1;
  while (imax >= imin)
    {
      const char *cur;

      imid = (imin + imax) / 2;

      g_autoptr(GVariant) child = g_variant_get_child_value (array, imid);
      g_variant_get_child (child, 0, "&s", &cur, NULL);

      int cmp = strcmp (cur, str);
      if (cmp < 0)
        imin = imid + 1;
      else if (cmp > 0)
        {
          if (imid == 0)
            break;
          imax = imid - 1;
        }
      else
        {
          if (imin < imid)
            imax = imid;
          else
            {
              *out_pos = imid;
              return TRUE;
            }
        }
    }

  *out_pos = imid;
  return FALSE;
}

const char*
rpmostree_auto_update_policy_to_str (RpmostreedAutomaticUpdatePolicy policy,
                                     GError **error)
{
  switch (policy)
    {
    case RPMOSTREED_AUTOMATIC_UPDATE_POLICY_NONE:
      return "none";
    case RPMOSTREED_AUTOMATIC_UPDATE_POLICY_CHECK:
      return "check";
    case RPMOSTREED_AUTOMATIC_UPDATE_POLICY_STAGE:
      return "stage";
    default:
      return glnx_null_throw (error, "Invalid policy value %u", policy);
    }
}

gboolean
rpmostree_str_to_auto_update_policy (const char *str,
                                     RpmostreedAutomaticUpdatePolicy *out_policy,
                                     GError    **error)
{
  g_assert (str);
  if (g_str_equal (str, "none") || g_str_equal (str, "off"))
    *out_policy = RPMOSTREED_AUTOMATIC_UPDATE_POLICY_NONE;
  else if (g_str_equal (str, "check"))
    *out_policy = RPMOSTREED_AUTOMATIC_UPDATE_POLICY_CHECK;
  else if (g_str_equal (str, "stage") || g_str_equal (str, "ex-stage") /* backcompat */)
    *out_policy = RPMOSTREED_AUTOMATIC_UPDATE_POLICY_STAGE;
  else
    return glnx_throw (error, "Invalid value for AutomaticUpdatePolicy: '%s'", str);
  return TRUE;
}

/* Get an ISO8601-formatted string for UTC timestamp t (seconds) */
char*
rpmostree_timestamp_str_from_unix_utc (guint64 t)
{
  g_autoptr(GDateTime) timestamp = g_date_time_new_from_unix_utc (t);
  if (timestamp != NULL)
    return g_date_time_format (timestamp, "%FT%H:%M:%SZ");
  return g_strdup_printf ("(invalid timestamp)");
}

gboolean
rpmostree_relative_path_is_ostree_compliant (const char *path)
{
  g_assert (path);
  g_assert (*path != '/');
  return (g_str_equal (path, "usr")   || (g_str_has_prefix (path, "usr/")
                                          && !g_str_has_prefix (path, "usr/local/")) ||
          g_str_equal (path, "bin")   || g_str_has_prefix (path, "bin/")  ||
          g_str_equal (path, "sbin")  || g_str_has_prefix (path, "sbin/") ||
          g_str_equal (path, "lib")   || g_str_has_prefix (path, "lib/")  ||
          g_str_equal (path, "lib64") || g_str_has_prefix (path, "lib64/"));
}
