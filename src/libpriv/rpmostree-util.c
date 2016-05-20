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
#include "libgsystem.h"
#include "libglnx.h"

void
_rpmostree_set_error_from_errno (GError    **error,
                                 gint        errsv)
{
  g_set_error_literal (error,
                       G_IO_ERROR,
                       g_io_error_from_errno (errsv),
                       g_strerror (errsv));
  errno = errsv;
}

void
_rpmostree_set_prefix_error_from_errno (GError     **error,
                                        gint         errsv,
                                        const char  *format,
                                        ...)
{
  gs_free char *formatted = NULL;
  va_list args;
  
  va_start (args, format);
  formatted = g_strdup_vprintf (format, args);
  va_end (args);
  
  _rpmostree_set_error_from_errno (error, errsv);
  g_prefix_error (error, "%s", formatted);
  errno = errsv;
}

void
_rpmostree_perror_fatal (const char *message)
{
  perror (message);
  exit (1);
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
  gs_unref_object GFileEnumerator *ret_direnum = NULL;

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
  gs_transfer_out_value (out_direnum, &ret_direnum);
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
  gs_free char *ret_contents = NULL;

  ret_contents = gs_file_load_contents_utf8 (path, cancellable, &temp_error);
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
  gs_transfer_out_value (out_contents, &ret_contents);
 out:
  return ret;
}

gboolean
_rpmostree_util_update_checksum_from_file (GChecksum    *checksum,
                                           GFile        *src,
                                           GCancellable *cancellable,
                                           GError      **error)
{
  gboolean ret = FALSE;
  gsize bytes_read;
  char buf[4096];
  gs_unref_object GInputStream *filein = NULL;

  filein = (GInputStream*)g_file_read (src, cancellable, error);
  if (!filein)
    goto out;

  do
    {
      if (!g_input_stream_read_all (filein, buf, sizeof(buf), &bytes_read,
                                    cancellable, error))
        goto out;

      g_checksum_update (checksum, (guint8*)buf, bytes_read);
    }
  while (bytes_read > 0);

  ret = TRUE;
 out:
  return ret;
}

static char *
ost_get_prev_commit (OstreeRepo *repo, char *checksum)
{
  char *ret = NULL;
  gs_unref_variant GVariant *commit = NULL;
  gs_unref_variant GVariant *parent_csum_v = NULL;
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
  gs_free char *beg_checksum = NULL;
  gs_free char *end_checksum = NULL;
  gs_free char *parent = NULL;
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
      _rpmostree_set_prefix_error_from_errno (error, errno, "waitpid: ");
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
  gs_free char *data = g_key_file_to_data (keyfile, &len, NULL);
  gboolean loaded;

  loaded = g_key_file_load_from_data (ret, data, len, 0, NULL);
  g_assert (loaded);
  return ret;
}

gboolean
_rpmostree_util_parse_origin (GKeyFile         *origin,
                              char            **out_refspec,
                              char           ***out_packages,
                              GError          **error)
{
  gboolean ret = FALSE;
  g_autofree char *origin_refspec = NULL;
  gboolean origin_is_bare_refspec = TRUE;

  origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
  if (!origin_refspec)
    {
      origin_refspec = g_key_file_get_string (origin, "origin", "baserefspec", NULL);
      origin_is_bare_refspec = FALSE;
    }
  if (!origin_refspec)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin/refspec or origin/baserefspec in current deployment origin; cannot upgrade via rpm-ostree");
      goto out;
    }

  if (out_refspec)
    *out_refspec = g_steal_pointer (&origin_refspec);

  if (out_packages)
    {
      if (origin_is_bare_refspec)
        *out_packages = NULL;
      else
        *out_packages = g_key_file_get_string_list (origin, "packages", "requested", NULL, NULL);
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_split_path_ptrarray_validate (const char *path,
                                        GPtrArray  **out_components,
                                        GError     **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) ret_components = NULL;

  if (strlen (path) > PATH_MAX)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Path '%s' is too long", path);
      goto out;
    }

  ret_components = g_ptr_array_new_with_free_func (g_free);

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
          goto out;
        }
      if (g_str_equal (component, ".") ||
          g_str_equal (component, ".."))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid special element '.' or '..' in path %s", path);
          goto out;
        }

      g_ptr_array_add (ret_components, (char*)g_steal_pointer (&component));
    } while (path && *path);

  ret = TRUE;
  *out_components = g_steal_pointer (&ret_components);
 out:
  return ret;
}
