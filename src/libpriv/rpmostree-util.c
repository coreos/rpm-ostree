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

#include "rpmostree-util.h"
#include "rpmostree-origin.h"
#include "libglnx.h"

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

gboolean
rpmostree_mkdtemp (const char   *template,
                   char        **out_tmpdir,
                   int          *out_tmpdir_dfd,  /* allow-none */
                   GError      **error)
{
  gboolean ret = FALSE;
  g_autofree char *tmpdir = g_strdup (template);
  gboolean created_tmpdir = FALSE;
  glnx_fd_close int ret_tmpdir_dfd = -1;

  if (mkdtemp (tmpdir) == NULL)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  created_tmpdir = TRUE;

  if (out_tmpdir_dfd)
    {
      if (!glnx_opendirat (AT_FDCWD, tmpdir, FALSE, &ret_tmpdir_dfd, error))
        goto out;
    }

  ret = TRUE;
  *out_tmpdir = g_steal_pointer (&tmpdir);
  if (out_tmpdir_dfd)
    {
      *out_tmpdir_dfd = ret_tmpdir_dfd;
      ret_tmpdir_dfd = -1;
    }
 out:
  if (created_tmpdir && tmpdir)
    {
     (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmpdir, NULL, NULL);
    }
  return ret;
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
  const char *s;
  const char *p;
  /* Acts as a reusable buffer space */
  g_autoptr(GString) varnamebuf = g_string_new ("");
  g_autoptr(GString) result = g_string_new ("");

  s = instr;
  while ((p = strstr (s, "${")) != NULL)
    {
      const char *varstart = p + 2;
      const char *varend = strchr (varstart, '}');
      const char *value;
      if (!varend)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unclosed variable reference starting at %u bytes",
                       (guint)(p - instr));
          return NULL;
        }

      /* Append leading bytes */
      g_string_append_len (result, s, p - s);

      /* Get a NUL-terminated copy of the variable name */
      g_string_truncate (varnamebuf, 0);
      g_string_append_len (varnamebuf, varstart, varend - varstart);

      value = g_hash_table_lookup (substitutions, varnamebuf->str);
      if (!value)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unknown variable reference ${%s}",
                       varnamebuf->str);
          return NULL;
        }
      /* Append the replaced value */
      g_string_append (result, value);

      /* On to the next */
      s = varend+1;
    }

  if (s != instr)
    {
      char *r;
      g_string_append_len (result, s, p - s);
      /* Steal the C string, NULL out the GString since we freed it */
      r = g_string_free (result, FALSE);
      result = NULL;
      return r;
    }
  else
    return g_strdup (instr);
}

gboolean
_rpmostree_util_enumerate_directory_allow_noent (GFile               *dirpath,
						 const char          *queryargs,
						 GFileQueryInfoFlags  queryflags,
						 GFileEnumerator    **out_direnum,
						 GCancellable        *cancellable,
						 GError             **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  g_autoptr(GFileEnumerator) ret_direnum = NULL;

  ret_direnum = g_file_enumerate_children (dirpath, queryargs, queryflags,
                                           cancellable, &temp_error);
  if (!ret_direnum)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret = TRUE;
        }
      else
        g_propagate_error (error, temp_error);

      goto out;
    }

  ret = TRUE;
  *out_direnum = g_steal_pointer (&ret_direnum);
 out:
  return ret;
}

gboolean
_rpmostree_file_load_contents_utf8_allow_noent (GFile          *path,
                                                char          **out_contents,
                                                GCancellable   *cancellable,
                                                GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  g_autofree char *ret_contents = NULL;

  ret_contents = glnx_file_get_contents_utf8_at (AT_FDCWD, gs_file_get_path_cached (path), NULL,
                                                 cancellable, &temp_error);
  if (!ret_contents)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  *out_contents = g_steal_pointer (&ret_contents);
 out:
  return ret;
}

gboolean
_rpmostree_util_update_checksum_from_file (GChecksum    *checksum,
                                           int           dfd,
                                           const char   *path,
                                           GCancellable *cancellable,
                                           GError      **error)
{
  glnx_fd_close int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;

  fd = openat (dfd, path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

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
  char *ret = NULL;
  g_autoptr(GVariant) commit = NULL;
  GError *tmp_error = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &commit, &tmp_error))
    goto out;

  ret = ostree_commit_get_parent (commit);

 out:
  g_clear_error (&tmp_error);

  return ret;
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

gboolean
_rpmostree_sync_wait_on_pid (pid_t          pid,
                             GError       **error)
{
  gboolean ret = FALSE;
  pid_t r;
  int estatus;

  do
    r = waitpid (pid, &estatus, 0);
  while (G_UNLIKELY (r == -1 && errno == EINTR));

  if (r == -1)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "waitpid");
      goto out;
    }

  if (!g_spawn_check_exit_status (estatus, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
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

GKeyFile *
_rpmostree_util_keyfile_clone (GKeyFile *keyfile)
{
  GKeyFile *ret = g_key_file_new ();
  gsize len;
  g_autofree char *data = g_key_file_to_data (keyfile, &len, NULL);
  gboolean loaded;

  loaded = g_key_file_load_from_data (ret, data, len, 0, NULL);
  g_assert (loaded);
  return ret;
}

gboolean
rpmostree_split_path_ptrarray_validate (const char *path,
                                        GPtrArray  **out_components,
                                        GError     **error)
{
  if (strlen (path) > PATH_MAX)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Path '%s' is too long", path);
      return FALSE;
    }

  g_autoptr(GPtrArray) ret_components = g_ptr_array_new_with_free_func (g_free);

  do
    {
      const char *p = strchr (path, '/');
      g_autofree char *component = NULL;

      if (!p)
        {
          component = g_strdup (path);
          path = NULL;
        }
      else
        {
          component = g_strndup (path, p - path);
          path = p + 1;
        }

      if (!component[0])
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid empty component in path '%s'", path);
          return FALSE;
        }
      if (g_str_equal (component, ".") ||
          g_str_equal (component, ".."))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid special element '.' or '..' in path %s", path);
          return FALSE;
        }

      g_ptr_array_add (ret_components, (char*)g_steal_pointer (&component));
    } while (path && *path);

  *out_components = g_steal_pointer (&ret_components);
  return TRUE;
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
rpmostree_deployment_get_layered_info (OstreeRepo        *repo,
                                       OstreeDeployment  *deployment,
                                       gboolean          *out_is_layered,
                                       char             **out_base_layer,
                                       char            ***out_layered_pkgs,
                                       GError           **error)
{
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GVariantDict) dict = NULL;
  g_auto(GStrv) layered_pkgs = NULL;
  const char *csum = ostree_deployment_get_csum (deployment);
  gboolean is_layered = FALSE;
  g_autofree char *base_layer = NULL;
  guint clientlayer_version = 0;

  if (!ostree_repo_load_commit (repo, csum, &commit, NULL, error))
    return FALSE;

  metadata = g_variant_get_child_value (commit, 0);
  dict = g_variant_dict_new (metadata);

  /* More recent versions have an explicit clientlayer attribute (which
   * realistically will always be TRUE). For older versions, we just
   * rely on the treespec being present. */
  if (!g_variant_dict_lookup (dict, "rpmostree.clientlayer", "b", &is_layered))
    is_layered = g_variant_dict_contains (dict, "rpmostree.spec");

  g_variant_dict_lookup (dict, "rpmostree.clientlayer_version", "u",
                         &clientlayer_version);

  /* only fetch base if we have to */
  if (is_layered && out_base_layer != NULL)
    {
      base_layer = ostree_commit_get_parent (commit);
      g_assert (base_layer);
    }

  /* only fetch the pkgs if we have to */
  if (is_layered && out_layered_pkgs != NULL)
    {
      /* starting from v1, we no longer embed a treespec in client layers */
      if (clientlayer_version > 0)
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
    }

  if (out_is_layered != NULL)
    *out_is_layered = is_layered;
  if (out_base_layer != NULL)
    *out_base_layer = g_steal_pointer (&base_layer);
  if (out_layered_pkgs != NULL)
    *out_layered_pkgs = g_steal_pointer (&layered_pkgs);

  return TRUE;
}
