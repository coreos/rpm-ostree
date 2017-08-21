/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 James Antill <james@and.org>
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
#include <glib-unix.h>
#include <gio/gunixoutputstream.h>
#include <stdio.h>
#include <json-glib/json-glib.h>

#include <pwd.h>
#include <grp.h>

#include "libglnx.h"
#include "rpmostree-util.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-passwd-util.h"

/* FIXME: */
#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

#include "libglnx.h"

static gboolean
dir_contains_uid_or_gid (GFile         *root,
                         guint32        id,
                         const char    *attr,
                         gboolean      *out_found_match,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean found_match = FALSE;

  /* zero it out, just to be sure */
  *out_found_match = found_match;

  g_autoptr(GFileInfo) file_info = g_file_query_info (root, OSTREE_GIO_FAST_QUERYINFO,
                                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                      cancellable, error);
  if (!file_info)
    return FALSE;

  GFileType type = g_file_info_get_file_type (file_info);
  guint32 tid;
  switch (type)
    {
    case G_FILE_TYPE_DIRECTORY:
    case G_FILE_TYPE_SYMBOLIC_LINK:
    case G_FILE_TYPE_REGULAR:
    case G_FILE_TYPE_SPECIAL:
      tid = g_file_info_get_attribute_uint32 (file_info, attr);
      if (tid == id)
        found_match = TRUE;
      break;

    case G_FILE_TYPE_UNKNOWN:
    case G_FILE_TYPE_SHORTCUT:
    case G_FILE_TYPE_MOUNTABLE:
      g_assert_not_reached ();
      break;
    }

  /* Now recurse for dirs. */
  if (!found_match && type == G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr(GFileEnumerator) dir_enum =
        g_file_enumerate_children (root, OSTREE_GIO_FAST_QUERYINFO,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                   NULL, error);
      if (!dir_enum)
        return FALSE;

      while (TRUE)
        {
          GFileInfo *file_info;
          GFile *child;

          if (!g_file_enumerator_iterate (dir_enum, &file_info, &child,
                                          cancellable, error))
            return FALSE;
          if (!file_info)
            break;

          if (!dir_contains_uid_or_gid (child, id, attr, &found_match,
                                        cancellable, error))
            return FALSE;

          if (found_match)
            break;
        }
    }

  *out_found_match = found_match;
  return TRUE;
}

static gboolean
dir_contains_uid (GFile           *yumroot,
                  uid_t            uid,
                  gboolean        *out_found_match,
                  GCancellable    *cancellable,
                  GError         **error)
{
  return dir_contains_uid_or_gid (yumroot, uid, "unix::uid",
                                  out_found_match, cancellable, error);
}

static gboolean
dir_contains_gid (GFile           *yumroot,
                  gid_t            gid,
                  gboolean        *out_found_match,
                  GCancellable    *cancellable,
                  GError         **error)
{
  return dir_contains_uid_or_gid (yumroot, gid, "unix::gid",
                                  out_found_match, cancellable, error);
}

static void
conv_passwd_ent_free (void *vptr)
{
  struct conv_passwd_ent *ptr = vptr;

  g_free (ptr->name);
  g_free (ptr);
}

GPtrArray *
rpmostree_passwd_data2passwdents (const char *data)
{
  struct passwd *ent = NULL;
  GPtrArray *ret = g_ptr_array_new_with_free_func (conv_passwd_ent_free);

  g_return_val_if_fail (data != NULL, NULL);

  g_autoptr(FILE) mf = fmemopen ((void *)data, strlen (data), "r");

  while ((ent = fgetpwent (mf)))
    {
      struct conv_passwd_ent *convent = g_new (struct conv_passwd_ent, 1);

      convent->name = g_strdup (ent->pw_name);
      convent->uid  = ent->pw_uid;
      convent->gid  = ent->pw_gid;
      /* Want to add anymore, like dir? */

      g_ptr_array_add (ret, convent);
    }

  return ret;
}

static int
compare_passwd_ents (gconstpointer a, gconstpointer b)
{
  const struct conv_passwd_ent **sa = (const struct conv_passwd_ent **)a;
  const struct conv_passwd_ent **sb = (const struct conv_passwd_ent **)b;

  return strcmp ((*sa)->name, (*sb)->name);
}

static void
conv_group_ent_free (void *vptr)
{
  struct conv_group_ent *ptr = vptr;

  g_free (ptr->name);
  g_free (ptr);
}

GPtrArray *
rpmostree_passwd_data2groupents (const char *data)
{
  g_return_val_if_fail (data != NULL, NULL);

  g_autoptr(FILE) mf = fmemopen ((void *)data, strlen (data), "r");
  GPtrArray *ret = g_ptr_array_new_with_free_func (conv_group_ent_free);
  struct group *ent = NULL;
  while ((ent = fgetgrent (mf)))
    {
      struct conv_group_ent *convent = g_new (struct conv_group_ent, 1);

      convent->name = g_strdup (ent->gr_name);
      convent->gid  = ent->gr_gid;
      /* Want to add anymore, like users? */

      g_ptr_array_add (ret, convent);
    }

  return ret;
}

static int
compare_group_ents (gconstpointer a, gconstpointer b)
{
  const struct conv_group_ent **sa = (const struct conv_group_ent **)a;
  const struct conv_group_ent **sb = (const struct conv_group_ent **)b;

  return strcmp ((*sa)->name, (*sb)->name);
}

/* See "man 5 passwd" We just make sure the name and uid/gid match,
   and that none are missing. don't care about GECOS/dir/shell.
*/
static gboolean
rpmostree_check_passwd_groups (gboolean         passwd,
                               OstreeRepo      *repo,
                               GFile           *yumroot,
                               GFile           *treefile_dirpath,
                               JsonObject      *treedata,
                               const char      *previous_commit,
                               GCancellable    *cancellable,
                               GError         **error)
{
  const char *direct = NULL;
  const char *chk_type = "previous";
  const char *commit_filepath = passwd ? "usr/lib/passwd" : "usr/lib/group";
  const char *json_conf_name  = passwd ? "check-passwd" : "check-groups";
  const char *json_conf_ign   = passwd ? "ignore-removed-users" : "ignore-removed-groups";
  g_autoptr(GFile) new_path = g_file_resolve_relative_path (yumroot, commit_filepath);
  g_autoptr(GPtrArray) ignore_removed_ents = NULL;
  gboolean ignore_all_removed = FALSE;
  g_autoptr(GPtrArray) old_ents = NULL;
  g_autoptr(GPtrArray) new_ents = NULL;

  if (json_object_has_member (treedata, json_conf_name))
    {
      JsonObject *chk = json_object_get_object_member (treedata,json_conf_name);
      if (!chk)
        return glnx_throw (error, "%s is not an object", json_conf_name);

      chk_type = _rpmostree_jsonutil_object_require_string_member (chk, "type", error);
      if (!chk_type)
        return FALSE;
      if (g_str_equal (chk_type, "none"))
        return TRUE; /* Note early return */

      else if (g_str_equal (chk_type, "file"))
        {
          direct = _rpmostree_jsonutil_object_require_string_member (chk,
                                                                     "filename",
                                                                     error);
          if (!direct)
            return FALSE;
        }
      else if (g_str_equal (chk_type, "data"))
        {
          JsonNode *ents_node = json_object_get_member (chk, "entries");
          JsonObject *ents_obj = NULL;
          GList *ents;
          GList *iter;

          if (!ents_node)
            return glnx_throw (error, "No entries member for data in %s", json_conf_name);

          ents_obj = json_node_get_object (ents_node);

          if (passwd)
            old_ents = g_ptr_array_new_with_free_func (conv_passwd_ent_free);
          else
            old_ents = g_ptr_array_new_with_free_func (conv_group_ent_free);

          ents = json_object_get_members (ents_obj);
          for (iter = ents; iter; iter = iter->next)
            if (passwd)
            {
              const char *name = iter->data;
              JsonNode *val = json_object_get_member (ents_obj, name);
              JsonNodeType child_type = json_node_get_node_type (val);
              gint64 uid = 0;
              gint64 gid = 0;
              struct conv_passwd_ent *convent = g_new (struct conv_passwd_ent, 1);

              if (child_type != JSON_NODE_ARRAY)
                {
                  if (!_rpmostree_jsonutil_object_require_int_member (ents_obj, name, &uid, error))
                    return FALSE;
                  gid = uid;
                }
              else
                {
                  JsonArray *child_array = json_node_get_array (val);
                  guint len = json_array_get_length (child_array);

                  if (!len || (len > 2))
                    return glnx_throw (error, "Array %s is only for uid and gid. Has length %u",
                                       name, len);
                  if (!_rpmostree_jsonutil_array_require_int_element (child_array, 0, &uid, error))
                    return FALSE;
                  if (len == 1)
                    gid = uid;
                  else if (!_rpmostree_jsonutil_array_require_int_element (child_array, 1, &gid, error))
                    return FALSE;
                }

              convent->name = g_strdup (name);
              convent->uid  = uid;
              convent->gid  = gid;
              g_ptr_array_add (old_ents, convent);
            }
            else
            {
              const char *name = iter->data;
              gint64 gid = 0;
              struct conv_group_ent *convent = g_new (struct conv_group_ent, 1);

              if (!_rpmostree_jsonutil_object_require_int_member (ents_obj, name, &gid, error))
                return FALSE;

              convent->name = g_strdup (name);
              convent->gid  = gid;
              g_ptr_array_add (old_ents, convent);
            }
        }
    }

  g_autoptr(GFile) old_path = NULL;
  g_autofree char *old_contents = NULL;
  if (g_str_equal (chk_type, "previous"))
    {
      if (previous_commit != NULL)
        {
          g_autoptr(GFile) root = NULL;

          if (!ostree_repo_read_commit (repo, previous_commit, &root, NULL, NULL, error))
            return FALSE;

          old_path = g_file_resolve_relative_path (root, commit_filepath);
          /* Note this one can't be ported to glnx_file_get_contents_utf8_at() because
           * we're loading from ostree via `OstreeRepoFile`.
           */
          if (!g_file_load_contents (old_path, cancellable, &old_contents, NULL, NULL, error))
            return FALSE;
        }
      else
        {
          /* Early return */
          return TRUE;
        }
    }
  else if (g_str_equal (chk_type, "file"))
    {
      old_path = g_file_resolve_relative_path (treefile_dirpath, direct);
      old_contents = glnx_file_get_contents_utf8_at (AT_FDCWD, gs_file_get_path_cached (old_path), NULL,
                                                     cancellable, error);
      if (!old_contents)
        return FALSE;
    }

  if (g_str_equal (chk_type, "previous") || g_str_equal (chk_type, "file"))
    {
      if (passwd)
        old_ents = rpmostree_passwd_data2passwdents (old_contents);
      else
        old_ents = rpmostree_passwd_data2groupents (old_contents);
    }
  g_assert (old_ents);

  if (passwd)
    g_ptr_array_sort (old_ents, compare_passwd_ents);
  else
    g_ptr_array_sort (old_ents, compare_group_ents);

  g_autofree char *new_contents =
    glnx_file_get_contents_utf8_at (AT_FDCWD, gs_file_get_path_cached (new_path), NULL,
                                    cancellable, error);
  if (!new_contents)
      return FALSE;

  if (json_object_has_member (treedata, json_conf_ign))
    {
      ignore_removed_ents = g_ptr_array_new ();
      if (!_rpmostree_jsonutil_append_string_array_to (treedata, json_conf_ign,
                                                       ignore_removed_ents,
                                                       error))
        return FALSE;
    }
  ignore_all_removed = rpmostree_str_ptrarray_contains (ignore_removed_ents, "*");

  if (passwd)
    {
      new_ents = rpmostree_passwd_data2passwdents (new_contents);
      g_ptr_array_sort (new_ents, compare_passwd_ents);
    }
  else
    {
      new_ents = rpmostree_passwd_data2groupents (new_contents);
      g_ptr_array_sort (new_ents, compare_group_ents);
    }

  unsigned int oiter = 0;
  unsigned int niter = 0;
  while ((oiter < old_ents->len) && (niter < new_ents->len))
    if (passwd)
    {
      struct conv_passwd_ent *odata = old_ents->pdata[oiter];
      struct conv_passwd_ent *ndata = new_ents->pdata[niter];
      int cmp = 0;

      cmp = g_strcmp0 (odata->name, ndata->name);

      if (cmp == 0)
        {
          if (odata->uid != ndata->uid)
            return glnx_throw (error, "passwd UID changed: %s (%u to %u)",
                               odata->name, (guint)odata->uid, (guint)ndata->uid);
          if (odata->gid != ndata->gid)
            return glnx_throw (error, "passwd GID changed: %s (%u to %u)",
                               odata->name, (guint)odata->gid, (guint)ndata->gid);

          ++oiter;
          ++niter;
        }
      else if (cmp < 0) /* Missing value from new passwd */
        {
          gboolean found_matching_uid;

          if (ignore_all_removed ||
              rpmostree_str_ptrarray_contains (ignore_removed_ents, odata->name))
            {
              g_print ("Ignored user missing from new passwd file: %s\n",
                       odata->name);
            }
          else
            {
              if (!dir_contains_uid (yumroot, odata->uid, &found_matching_uid,
                                     cancellable, error))
                return FALSE;

              if (found_matching_uid)
                return glnx_throw (error, "User missing from new passwd file: %s", odata->name);
              else
                g_print ("User removed from new passwd file: %s\n",
                         odata->name);
            }

          ++oiter;
        }
      else
        {
          g_print ("New passwd entry: %s\n", ndata->name);
          ++niter;
        }
    }
    else
    {
      struct conv_group_ent *odata = old_ents->pdata[oiter];
      struct conv_group_ent *ndata = new_ents->pdata[niter];
      int cmp = 0;

      cmp = g_strcmp0 (odata->name, ndata->name);

      if (cmp == 0)
        {
          if (odata->gid != ndata->gid)
            return glnx_throw (error, "group GID changed: %s (%u to %u)",
                               odata->name, (guint)odata->gid, (guint)ndata->gid);

          ++oiter;
          ++niter;
          continue;
        }
      else if (cmp < 0) /* Missing value from new group */
        {

          if (ignore_all_removed ||
              rpmostree_str_ptrarray_contains (ignore_removed_ents, odata->name))
            {
              g_print ("Ignored group missing from new group file: %s\n",
                       odata->name);
            }
          else
            {
              gboolean found_gid;

              if (!dir_contains_gid (yumroot, odata->gid, &found_gid,
                                     cancellable, error))
                return FALSE;

              if (found_gid)
                return glnx_throw (error, "Group missing from new group file: %s", odata->name);
              else
                g_print ("Group removed from new passwd file: %s\n",
                         odata->name);
            }

          ++oiter;
        }
      else
        {
          g_print ("New group entry: %s\n", ndata->name);
          ++niter;
        }
    }

  if (passwd)
    {
      if (oiter < old_ents->len)
        {
          struct conv_passwd_ent *odata = old_ents->pdata[oiter];

          return glnx_throw (error, "User missing from new passwd file: %s", odata->name);
        }

      while (niter < new_ents->len)
        {
          struct conv_passwd_ent *ndata = new_ents->pdata[niter];

          g_print ("New passwd entry: %s\n", ndata->name);
          ++niter;
        }
    }
  else
    {
      if (oiter < old_ents->len)
        {
          struct conv_group_ent *odata = old_ents->pdata[oiter];

          return glnx_throw (error, "Group missing from new group file: %s", odata->name);
        }

      while (niter < new_ents->len)
        {
          struct conv_group_ent *ndata = new_ents->pdata[niter];

          g_print ("New group entry: %s\n", ndata->name);
          ++niter;
        }
    }

  return TRUE;
}

/* See "man 5 passwd" We just make sure the name and uid/gid match,
   and that none are missing. don't care about GECOS/dir/shell.
*/
gboolean
rpmostree_check_passwd (OstreeRepo      *repo,
                        GFile           *yumroot,
                        GFile           *treefile_dirpath,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error)
{
  return rpmostree_check_passwd_groups (TRUE, repo, yumroot, treefile_dirpath,
                                        treedata, previous_commit,
                                        cancellable, error);
}

/* See "man 5 group" We just need to make sure the name and gid match,
   and that none are missing. Don't care about users.
 */
gboolean
rpmostree_check_groups (OstreeRepo      *repo,
                        GFile           *yumroot,
                        GFile           *treefile_dirpath,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error)
{
  return rpmostree_check_passwd_groups (TRUE, repo, yumroot, treefile_dirpath,
                                        treedata, previous_commit,
                                        cancellable, error);
}

static FILE *
gfopen (const char       *path,
        const char       *mode,
        GCancellable     *cancellable,
        GError          **error)
{
  FILE *ret = fopen (path, mode);
  if (!ret)
    return glnx_null_throw_errno_prefix (error, "fopen(%s)", path);
  return ret;
}

/*
 * This function is taking the /etc/passwd generated in the install
 * root, and splitting it into two streams: a new /etc/passwd that
 * just contains the root entry, and /usr/lib/passwd which contains
 * everything else.
 *
 * The implementation is kind of horrible because I wanted to avoid
 * duplicating the user/group code.
 */
gboolean
rpmostree_passwd_migrate_except_root (GFile         *rootfs,
                                      RpmOstreePasswdMigrateKind    kind,
                                      GHashTable    *preserve,
                                      GCancellable  *cancellable,
                                      GError       **error)
{
  const char *name = kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD ? "passwd" : "group";

  g_autofree char *src_path = g_strconcat (gs_file_get_path_cached (rootfs), "/etc/", name, NULL);
  g_autoptr(FILE) src_stream = gfopen (src_path, "r", cancellable, error);
  if (!src_stream)
    return FALSE;

  g_autofree char *etctmp_path = g_strconcat (gs_file_get_path_cached (rootfs), "/etc/", name, ".tmp", NULL);
  g_autoptr(FILE) etcdest_stream = gfopen (etctmp_path, "w", cancellable, error);
  if (!etcdest_stream)
    return FALSE;

  g_autofree char *usrdest_path = g_strconcat (gs_file_get_path_cached (rootfs), "/usr/lib/", name, NULL);
  g_autoptr(FILE) usrdest_stream = gfopen (usrdest_path, "a", cancellable, error);
  if (!usrdest_stream)
    return FALSE;

  errno = 0;
  while (TRUE)
    {
      struct passwd *pw = NULL;
      struct group *gr = NULL;

      if (kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD)
        pw = fgetpwent (src_stream);
      else
        gr = fgetgrent (src_stream);

      if (!(pw || gr))
        {
          if (errno != 0 && errno != ENOENT)
            return glnx_throw_errno_prefix (error, "fgetpwent");
          else
            break;
        }

      guint32 id;
      const char *name;
      if (pw)
        {
          id = pw->pw_uid;
          name = pw->pw_name;
        }
      else
        {
          id = gr->gr_gid;
          name = gr->gr_name;
        }

      FILE *deststream;
      if (id == 0)
        deststream = etcdest_stream;
      else
        deststream = usrdest_stream;

      int r;
      if (pw)
        r = putpwent (pw, deststream);
      else
        r = putgrent (gr, deststream);

      /* If it's marked in the preserve group, we need to write to
       * *both* /etc and /usr/lib in order to preserve semantics for
       * upgraded systems from before we supported the preserve
       * concept.
       */
      if (preserve && g_hash_table_contains (preserve, name))
        {
          /* We should never be trying to preserve the root entry, it
           * should always be only in /etc.
           */
          g_assert (deststream == usrdest_stream);
          if (pw)
            r = putpwent (pw, etcdest_stream);
          else
            r = putgrent (gr, etcdest_stream);
        }

      if (r == -1)
        return glnx_throw_errno_prefix (error, "putpwent");
    }

  if (!glnx_stdio_file_flush (etcdest_stream, error))
    return FALSE;
  if (!glnx_stdio_file_flush (usrdest_stream, error))
    return FALSE;

  if (rename (etctmp_path, src_path) != 0)
    return glnx_throw_errno_prefix (error, "rename");

  return TRUE;
}

static FILE *
target_etc_filename (GFile         *yumroot,
                     const char    *filename,
                     GCancellable  *cancellable,
                     GError       **error)
{
  g_autofree char *etc_subpath = g_strconcat ("etc/", filename, NULL);
  g_autofree char *target_etc =
    g_build_filename (gs_file_get_path_cached (yumroot), etc_subpath, NULL);
  return gfopen (target_etc, "w", cancellable, error);
}

static gboolean
_rpmostree_gfile2stdio (GFile         *source,
                        char         **storage_buf,
                        FILE         **ret_src_stream,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gsize len;

  /* We read the file into memory using Gio (which talks
   * to libostree), then memopen it, which works with libc.
   */
  if (!g_file_load_contents (source, cancellable,
                             storage_buf, &len, NULL, error))
    return FALSE;

  if (len == 0)
    {
      *ret_src_stream = NULL;
      return TRUE; /* Early return */
    }

  FILE *src_stream = fmemopen (*storage_buf, len, "r");
  if (!src_stream)
    return glnx_throw_errno_prefix (error, "fmemopen");

  *ret_src_stream = g_steal_pointer (&src_stream);
  return TRUE;
}

static gboolean
concat_entries (FILE    *src_stream,
                FILE    *dest_stream,
                RpmOstreePasswdMigrateKind kind,
                GHashTable *seen_names,
                GError **error)
{
  errno = 0;
  while (TRUE)
    {
      struct passwd *pw = NULL;
      struct group *gr = NULL;
      int r;
      const char *name;

      if (kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD)
        pw = fgetpwent (src_stream);
      else
        gr = fgetgrent (src_stream);

      if (!(pw || gr))
        {
          if (errno != 0 && errno != ENOENT)
            return glnx_throw_errno_prefix (error, "fgetpwent");
          else
            break;
        }

      if (pw)
        name = pw->pw_name;
      else
        name = gr->gr_name;

      /* Deduplicate */
      if (g_hash_table_lookup (seen_names, name))
        continue;
      g_hash_table_add (seen_names, g_strdup (name));

      if (pw)
        r = putpwent (pw, dest_stream);
      else
        r = putgrent (gr, dest_stream);

      if (r == -1)
        return glnx_throw_errno_prefix (error, "putpwent");
    }

  return TRUE;
}

static gboolean
concat_passwd_file (GFile           *yumroot,
                    GFile           *previous_commit,
                    RpmOstreePasswdMigrateKind kind,
                    GCancellable    *cancellable,
                    GError         **error)
{
  const char *filename = kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD ? "passwd" : "group";
  g_autofree char *usretc_subpath = g_strconcat ("usr/etc/", filename, NULL);
  g_autofree char *usrlib_subpath = g_strconcat ("usr/lib/", filename, NULL);
  g_autoptr(GFile) orig_etc_content =
    g_file_resolve_relative_path (previous_commit, usretc_subpath);
  g_autoptr(GFile) orig_usrlib_content =
    g_file_resolve_relative_path (previous_commit, usrlib_subpath);
  g_autoptr(GHashTable) seen_names =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  GFile *sources[] = { orig_etc_content, orig_usrlib_content };

  gboolean have_etc = g_file_query_exists (orig_etc_content, NULL);
  gboolean have_usr = g_file_query_exists (orig_usrlib_content, NULL);

  /* This could actually happen after we transition to
   * systemd-sysusers; we won't have a need for preallocated user data
   * in the tree.
   */
  if (!(have_etc || have_usr))
    return TRUE;

  g_autoptr(FILE) dest_stream = NULL;
  if (!(dest_stream = target_etc_filename (yumroot, filename,
                                           cancellable, error)))
    return FALSE;

  for (guint i = 0; i < G_N_ELEMENTS (sources); i++)
    {
      GFile *source = sources[i];
      g_autoptr(FILE) src_stream = NULL;

      g_autofree char *contents = NULL;
      if (!_rpmostree_gfile2stdio (source, &contents, &src_stream,
                                   cancellable, error))
        return FALSE;

      if (!src_stream)
        continue;

      if (!concat_entries (src_stream, dest_stream, kind,
                           seen_names, error))
        return FALSE;
    }

  if (!glnx_stdio_file_flush (dest_stream, error))
    return FALSE;

  return TRUE;
}

static gboolean
_data_from_json (GFile           *yumroot,
                 GFile           *treefile_dirpath,
                 JsonObject      *treedata,
                 RpmOstreePasswdMigrateKind kind,
                 gboolean        *out_found,
                 GCancellable    *cancellable,
                 GError         **error)
{
  const gboolean passwd = (kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD);
  const char *json_conf_name = passwd ? "check-passwd" : "check-groups";
  *out_found = FALSE;
  if (!json_object_has_member (treedata, json_conf_name))
    return TRUE;

  JsonObject *chk = json_object_get_object_member (treedata,json_conf_name);
  if (!chk)
    return TRUE;

  const char *chk_type =
    _rpmostree_jsonutil_object_require_string_member (chk, "type", error);
  if (!chk_type)
    return FALSE;

  if (!g_str_equal (chk_type, "file"))
    return TRUE;

  const char *filename =
    _rpmostree_jsonutil_object_require_string_member (chk, "filename", error);
  if (!filename)
    return FALSE;

  g_autoptr(GFile) source = g_file_resolve_relative_path (treefile_dirpath, filename);
  if (!source)
    return FALSE;

  /* migrate the check data from the specified file to /etc */
  g_autoptr(FILE) src_stream = NULL;
  g_autofree char *contents = NULL;
  if (!_rpmostree_gfile2stdio (source, &contents, &src_stream,
                               cancellable, error))
    return FALSE;

  if (!src_stream)
    return TRUE;

  /* no matter what we've used the data now */
  *out_found = TRUE;

  g_autoptr(FILE) dest_stream = NULL;
  const char *filebasename = passwd ? "passwd" : "group";
  if (!(dest_stream = target_etc_filename (yumroot, filebasename,
                                           cancellable, error)))
    return FALSE;

  g_autoptr(GHashTable) seen_names =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  if (!concat_entries (src_stream, dest_stream, kind, seen_names, error))
    return FALSE;

  return TRUE;
}

gboolean
rpmostree_generate_passwd_from_previous (OstreeRepo      *repo,
                                         int              rootfs_dfd,
                                         GFile           *treefile_dirpath,
                                         GFile           *previous_root,
                                         JsonObject      *treedata,
                                         GCancellable    *cancellable,
                                         GError         **error)
{
  gboolean found_passwd_data = FALSE;
  gboolean found_groups_data = FALSE;
  gboolean perform_migrate = FALSE;
  g_autofree char *rootfs_abspath = glnx_fdrel_abspath (rootfs_dfd, ".");
  g_autoptr(GFile) yumroot = g_file_new_for_path (rootfs_abspath);

  /* Create /etc in the target root; FIXME - should ensure we're using
   * the right permissions from the filesystem RPM.  Doing this right
   * is really hard because filesystem depends on setup which installs
   * the files...
   */
  if (!glnx_ensure_dir (rootfs_dfd, "etc", 0755, error))
    return FALSE;

  if (!_data_from_json (yumroot, treefile_dirpath,
                        treedata, RPM_OSTREE_PASSWD_MIGRATE_PASSWD,
                        &found_passwd_data, cancellable, error))
    return FALSE;
  perform_migrate = !found_passwd_data;

  if (!previous_root)
    perform_migrate = FALSE;

  if (perform_migrate && !concat_passwd_file (yumroot, previous_root,
                                              RPM_OSTREE_PASSWD_MIGRATE_PASSWD,
                                              cancellable, error))
    return FALSE;

  if (!_data_from_json (yumroot, treefile_dirpath,
                        treedata, RPM_OSTREE_PASSWD_MIGRATE_GROUP,
                        &found_groups_data, cancellable, error))
    return FALSE;

  perform_migrate = !found_groups_data;

  if (!previous_root)
    perform_migrate = FALSE;

  if (perform_migrate && !concat_passwd_file (yumroot, previous_root,
                                              RPM_OSTREE_PASSWD_MIGRATE_GROUP,
                                              cancellable, error))
    return FALSE;

  /* We should error if we are getting passwd data from JSON and group from
   * previous commit, or vice versa, as that'll confuse everyone when it goes
   * wrong. */
  if ( found_passwd_data && !found_groups_data)
    return glnx_throw (error, "Configured to migrate passwd data from JSON, and group data from commit");
  if (!found_passwd_data &&  found_groups_data)
    return glnx_throw (error, "Configured to migrate passwd data from commit, and group data from JSON");

  return TRUE;
}

static const char *usrlib_pwgrp_files[] = { "passwd", "group" };
/* Lock/backup files that should not be in the base commit (TODO fix) */
static const char *pwgrp_lock_and_backup_files[] = { ".pwd.lock", "passwd-", "group-",
                                                     "shadow-", "gshadow-",
                                                     "subuid-", "subgid-" };
static const char *pwgrp_shadow_files[] = { "shadow", "gshadow",
                                            "subuid", "subgid"};

static gboolean
rootfs_has_usrlib_passwd (int rootfs_dfd,
                       gboolean *out_have_passwd,
                       GError **error)
{
  struct stat stbuf;

  /* Does this rootfs have a usr/lib/passwd?  We might be doing a
   * container or something else.
   */
  if (fstatat (rootfs_dfd, "usr/lib/passwd", &stbuf, 0) < 0)
    {
      if (errno == ENOENT)
        {
          *out_have_passwd = FALSE;
          return TRUE;
        }
      else
        return glnx_throw_errno_prefix (error, "fstatat");
    }
  *out_have_passwd = TRUE;
  return TRUE;
}

/* This may be leftover in the tree from an older version of rpm-ostree that
 * didn't clean them up at compose time, and having them exist will mean
 * rofiles-fuse will prevent useradd from opening it for write.
 */
gboolean
rpmostree_passwd_cleanup (int rootfs_dfd, GCancellable *cancellable, GError **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (pwgrp_lock_and_backup_files); i++)
    {
      const char *file = pwgrp_lock_and_backup_files[i];
      if (unlinkat (rootfs_dfd, glnx_strjoina ("usr/etc/", file), 0) < 0)
        {
          if (errno == ENOENT)
            ;
          else
            return glnx_throw_errno_prefix (error, "unlinkat");
        }
    }

  return TRUE;
}

/* We actually want RPM to inject to /usr/lib/passwd - we
 * accomplish this by temporarily renaming /usr/lib/passwd -> /usr/etc/passwd
 * (Which appears as /etc/passwd via our compatibility symlink in the bubblewrap
 *  script runner). We also copy the merge deployment's /etc/passwd to
 *  /usr/lib/passwd, so that %pre scripts are aware of newly added system users
 *  not in the tree's /usr/lib/passwd (through nss-altfiles in the container).
 */
gboolean
rpmostree_passwd_prepare_rpm_layering (int                rootfs_dfd,
                                       const char        *merge_passwd_dir,
                                       gboolean          *out_have_passwd,
                                       GCancellable      *cancellable,
                                       GError           **error)
{
  if (!rpmostree_passwd_cleanup (rootfs_dfd, cancellable, error))
    return FALSE;

  if (!rootfs_has_usrlib_passwd (rootfs_dfd, out_have_passwd, error))
    return FALSE;
  if (!*out_have_passwd)
    return TRUE;

  for (guint i = 0; i < G_N_ELEMENTS (usrlib_pwgrp_files); i++)
    {
      const char *file = usrlib_pwgrp_files[i];
      const char *usrlibfile = glnx_strjoina ("usr/lib/", file);
      const char *usretcfile = glnx_strjoina ("usr/etc/", file);
      const char *usrlibfiletmp = glnx_strjoina ("usr/lib/", file, ".tmp");

      /* Retain the current copies in /etc as backups */
      if (!glnx_renameat (rootfs_dfd, usretcfile, rootfs_dfd,
                          glnx_strjoina (usretcfile, ".rpmostreesave"), error))
        return FALSE;

      /* Copy /usr/lib/{passwd,group} -> /usr/etc (breaking hardlinks) */
      if (!glnx_file_copy_at (rootfs_dfd, usrlibfile, NULL,
                              rootfs_dfd, usretcfile, 0, cancellable, error))
        return FALSE;

      /* Copy the merge's passwd/group to usr/lib (breaking hardlinks) */
      if (!glnx_file_copy_at (AT_FDCWD,
                              glnx_strjoina (merge_passwd_dir, "/", file), NULL,
                              rootfs_dfd, usrlibfiletmp,
                              GLNX_FILE_COPY_OVERWRITE, cancellable, error))
        return FALSE;

      if (!glnx_renameat (rootfs_dfd, usrlibfiletmp, rootfs_dfd, usrlibfile, error))
        return FALSE;
    }

  /* And break hardlinks for the shadow files, since we don't have
   * them in /usr/lib right now.
   */
  for (guint i = 0; i < G_N_ELEMENTS (pwgrp_shadow_files); i++)
    {
      struct stat stbuf;
      const char *file = pwgrp_shadow_files[i];
      const char *src = glnx_strjoina ("usr/etc/", file);
      const char *tmp = glnx_strjoina ("usr/etc/", file, ".tmp");

      if (fstatat (rootfs_dfd, src, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
        {
          if (errno != ENOENT)
            return glnx_throw_errno_prefix (error, "fstatat");
          continue;
        }

      if (!glnx_file_copy_at (rootfs_dfd, src, NULL,
                              rootfs_dfd, tmp, GLNX_FILE_COPY_OVERWRITE,
                              cancellable, error))
        return FALSE;
      if (!glnx_renameat (rootfs_dfd, tmp, rootfs_dfd, src, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
rpmostree_passwd_complete_rpm_layering (int       rootfs_dfd,
                                        GError  **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (usrlib_pwgrp_files); i++)
    {
      const char *file = usrlib_pwgrp_files[i];
      /* And now the inverse: /usr/etc/passwd -> /usr/lib/passwd */
      if (!glnx_renameat (rootfs_dfd, glnx_strjoina ("usr/etc/", file),
                          rootfs_dfd, glnx_strjoina ("usr/lib/", file), error))
        return FALSE;
      /* /usr/etc/passwd.rpmostreesave -> /usr/etc/passwd */
      if (!glnx_renameat (rootfs_dfd, glnx_strjoina ("usr/etc/", file, ".rpmostreesave"),
                          rootfs_dfd, glnx_strjoina ("usr/etc/", file), error))
        return FALSE;
    }
  /* However, we leave the (potentially modified) shadow files in place.
   * In actuality, nothing should change /etc/shadow or /etc/gshadow, so
   * we'll just have to pay the (tiny) cost of re-checksumming.
   */
  return TRUE;
}
