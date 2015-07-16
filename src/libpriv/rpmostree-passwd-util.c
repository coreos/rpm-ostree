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

#include "rpmostree-util.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-passwd-util.h"

/* FIXME: */
#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

#include "libgsystem.h"

GS_DEFINE_CLEANUP_FUNCTION0(FILE*, _cleanup_stdio_file, fclose);
#define _cleanup_stdio_file_ __attribute__((cleanup(_cleanup_stdio_file)))


static gboolean
ptrarray_contains_str (GPtrArray *haystack, const char *needle)
{
  /* faster if sorted+bsearch ... but probably doesn't matter */
  guint i;

  if (!haystack || !needle)
    return FALSE;

  for (i = 0; i < haystack->len; i++)
    {
      const char *data = haystack->pdata[i];

      if (g_str_equal (data, needle))
        return TRUE;
    }

  return FALSE;
}

static gboolean
dir_contains_uid_or_gid (GFile         *root,
                         guint32        id,
                         const char    *attr,
                         gboolean      *out_found_match,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileInfo *file_info = NULL;
  guint32 type;
  guint32 tid;
  gboolean found_match = FALSE;

  /* zero it out, just to be sure */
  *out_found_match = found_match;

  file_info = g_file_query_info (root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!file_info)
    goto out;

  type = g_file_info_get_file_type (file_info);

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
      gs_unref_object GFileEnumerator *dir_enum = NULL;
      gs_unref_object GFileInfo *child_info = NULL;

      dir_enum = g_file_enumerate_children (root, OSTREE_GIO_FAST_QUERYINFO, 
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            NULL, 
                                            error);
      if (!dir_enum)
        goto out;
  
      while (TRUE)
        {
          GFileInfo *file_info;
          GFile *child;

          if (!gs_file_enumerator_iterate (dir_enum, &file_info, &child,
                                           cancellable, error))
            goto out;
          if (!file_info)
            break;

          if (!dir_contains_uid_or_gid (child, id, attr, &found_match,
                                        cancellable, error))
            goto out;

          if (found_match)
            break;
        }
    }
  
  ret = TRUE;
  *out_found_match = found_match;
 out:
  return ret;
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

struct conv_passwd_ent
{
  char *name;
  uid_t uid;
  gid_t gid;
};

static void
conv_passwd_ent_free (void *vptr)
{
  struct conv_passwd_ent *ptr = vptr;

  g_free (ptr->name);
  g_free (ptr);
}

static GPtrArray *
data2passwdents (const char *data)
{
  struct passwd *ent = NULL;
  _cleanup_stdio_file_ FILE *mf = NULL;
  GPtrArray *ret = g_ptr_array_new_with_free_func (conv_passwd_ent_free);

  mf = fmemopen ((void *)data, strlen (data), "r");

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

struct conv_group_ent
{
  char *name;
  gid_t gid;
};

static void
conv_group_ent_free (void *vptr)
{
  struct conv_group_ent *ptr = vptr;

  g_free (ptr->name);
  g_free (ptr);
}

static GPtrArray *
data2groupents (const char *data)
{
  struct group *ent = NULL;
  _cleanup_stdio_file_ FILE *mf = NULL;
  GPtrArray *ret = g_ptr_array_new_with_free_func (conv_group_ent_free);
  
  mf = fmemopen ((void *)data, strlen (data), "r");
  
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
                               GCancellable    *cancellable,
                               GError         **error)
{
  gboolean ret = FALSE;
  const char *direct = NULL;
  const char *chk_type = "previous";
  const char *ref = NULL;
  const char *commit_filepath = passwd ? "usr/lib/passwd" : "usr/lib/group";
  const char *json_conf_name  = passwd ? "check-passwd" : "check-groups";
  const char *json_conf_ign   = passwd ? "ignore-removed-users" : "ignore-removed-groups";
  gs_unref_object GFile *old_path = NULL;
  gs_unref_object GFile *new_path = g_file_resolve_relative_path (yumroot, commit_filepath);
  gs_unref_ptrarray GPtrArray *ignore_removed_ents = NULL;
  gboolean ignore_all_removed = FALSE;
  gs_free char *old_contents = NULL;
  gs_free char *new_contents = NULL;
  gs_unref_ptrarray GPtrArray *old_ents = NULL;
  gs_unref_ptrarray GPtrArray *new_ents = NULL;
  unsigned int oiter = 0;
  unsigned int niter = 0;

  if (json_object_has_member (treedata, json_conf_name))
    {
      JsonObject *chk = json_object_get_object_member (treedata,json_conf_name);
      if (!chk)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "%s is not an object", json_conf_name);
          goto out;
        }

      chk_type = _rpmostree_jsonutil_object_require_string_member (chk, "type",
                                                                   error);
      if (!chk_type)
        goto out;
      if (g_str_equal (chk_type, "none"))
        {
          ret = TRUE;
          goto out;
        }
      else if (g_str_equal (chk_type, "file"))
        {
          direct = _rpmostree_jsonutil_object_require_string_member (chk,
                                                                     "filename",
                                                                     error);
          if (!direct)
            goto out;
        }
      else if (g_str_equal (chk_type, "data"))
        {
          JsonNode *ents_node = json_object_get_member (chk, "entries");
          JsonObject *ents_obj = NULL;
          GList *ents;
          GList *iter;

          if (!ents_node)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No entries member for data in %s", json_conf_name);
              goto out;
            }

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
                    goto out;
                  gid = uid;
                }
              else
                {
                  JsonArray *child_array = json_node_get_array (val);
                  guint len = json_array_get_length (child_array);

                  if (!len || (len > 2))
                    {
                      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Array %s is only for uid and gid. Has length %u",
                                   name, len);
                      goto out;
                    }
                  if (!_rpmostree_jsonutil_array_require_int_element (child_array, 0, &uid, error))
                    goto out;
                  if (len == 1)
                    gid = uid;
                  else if (!_rpmostree_jsonutil_array_require_int_element (child_array, 1, &gid, error))
                    goto out;
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
                goto out;

              convent->name = g_strdup (name);
              convent->gid  = gid;
              g_ptr_array_add (old_ents, convent);
            }
        }
    }

  if (g_str_equal (chk_type, "previous"))
    {
      gs_unref_object GFile *root = NULL;
      GError *tmp_error = NULL;

      ref = _rpmostree_jsonutil_object_require_string_member (treedata, "ref",
                                                              error);
      if (!ref)
        goto out;

      if (!ostree_repo_read_commit (repo, ref, &root, NULL, NULL, &tmp_error))
        {
          if (g_error_matches (tmp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            { /* this is kind of a hack, makes it work if it's the first commit */
              g_clear_error (&tmp_error);
              ret = TRUE;
            }
          else
            {
              g_propagate_error (error, tmp_error);
            }
          goto out;
        }

      old_path = g_file_resolve_relative_path (root, commit_filepath);
    }

  if (g_str_equal (chk_type, "file"))
    {
      old_path = g_file_resolve_relative_path (treefile_dirpath, direct);
    }

  if (g_str_equal (chk_type, "previous") || g_str_equal (chk_type, "file"))
    {
      old_contents = gs_file_load_contents_utf8 (old_path, cancellable, error);
      if (!old_contents)
        goto out;
      if (passwd)
        old_ents = data2passwdents (old_contents);
      else
        old_ents = data2groupents (old_contents);
    }
  g_assert (old_ents);

  if (passwd)
    g_ptr_array_sort (old_ents, compare_passwd_ents);
  else
    g_ptr_array_sort (old_ents, compare_group_ents);

  new_contents = gs_file_load_contents_utf8 (new_path, cancellable, error);
  if (!new_contents)
      goto out;

  if (json_object_has_member (treedata, json_conf_ign))
    {
      ignore_removed_ents = g_ptr_array_new ();
      if (!_rpmostree_jsonutil_append_string_array_to (treedata, json_conf_ign,
                                                       ignore_removed_ents,
                                                       error))
        goto out;
    }
  ignore_all_removed = ptrarray_contains_str (ignore_removed_ents, "*");

  if (passwd)
    {
      new_ents = data2passwdents (new_contents);
      g_ptr_array_sort (new_ents, compare_passwd_ents);
    }
  else
    {
      new_ents = data2groupents (new_contents);
      g_ptr_array_sort (new_ents, compare_group_ents);
    }

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
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "passwd UID changed: %s (%u to %u)",
                           odata->name, (guint)odata->uid, (guint)ndata->uid);
              goto out;
            }
          if (odata->gid != ndata->gid)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "passwd GID changed: %s (%u to %u)",
                           odata->name, (guint)odata->gid, (guint)ndata->gid);
              goto out;
            }

          ++oiter;
          ++niter;
        }
      else if (cmp < 0) /* Missing value from new passwd */
        {
          gboolean found_matching_uid;

          if (ignore_all_removed ||
              ptrarray_contains_str (ignore_removed_ents, odata->name))
            {
              g_print ("Ignored user missing from new passwd file: %s\n",
                       odata->name);
            }
          else
            {
              if (!dir_contains_uid (yumroot, odata->uid, &found_matching_uid,
                                     cancellable, error))
                goto out;

              if (found_matching_uid)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "User missing from new passwd file: %s", odata->name);
                  goto out;
                }
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
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "group GID changed: %s (%u to %u)",
                           odata->name, (guint)odata->gid, (guint)ndata->gid);
              goto out;
            }

          ++oiter;
          ++niter;
          continue;
        }
      else if (cmp < 0) /* Missing value from new group */
        {

          if (ignore_all_removed ||
              ptrarray_contains_str (ignore_removed_ents, odata->name))
            {
              g_print ("Ignored group missing from new group file: %s\n",
                       odata->name);
            }
          else
            {
              gboolean found_gid;

              if (!dir_contains_gid (yumroot, odata->gid, &found_gid,
                                     cancellable, error))
                goto out;

              if (found_gid)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Group missing from new group file: %s", odata->name);
                  goto out;
                }
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

          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "User missing from new passwd file: %s", odata->name);
          goto out;
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

          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Group missing from new group file: %s", odata->name);
          goto out;
        }

      while (niter < new_ents->len)
        {
          struct conv_group_ent *ndata = new_ents->pdata[niter];

          g_print ("New group entry: %s\n", ndata->name);
          ++niter;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/* See "man 5 passwd" We just make sure the name and uid/gid match,
   and that none are missing. don't care about GECOS/dir/shell.
*/
gboolean
rpmostree_check_passwd (OstreeRepo      *repo,
                        GFile           *yumroot,
                        GFile           *treefile_dirpath,
                        JsonObject      *treedata,
                        GCancellable    *cancellable,
                        GError         **error)
{
  return rpmostree_check_passwd_groups (TRUE, repo, yumroot, treefile_dirpath,
                                        treedata, cancellable, error);
}

/* See "man 5 group" We just need to make sure the name and gid match,
   and that none are missing. Don't care about users.
 */
gboolean
rpmostree_check_groups (OstreeRepo      *repo,
                        GFile           *yumroot,
                        GFile           *treefile_dirpath,
                        JsonObject      *treedata,
                        GCancellable    *cancellable,
                        GError         **error)
{
  return rpmostree_check_passwd_groups (TRUE, repo, yumroot, treefile_dirpath,
                                        treedata, cancellable, error);
}

static FILE *
gfopen (const char       *path,
        const char       *mode,
        GCancellable     *cancellable,
        GError          **error)
{
  FILE *ret = NULL; 

  ret = fopen (path, mode);
  if (!ret)
    {
      _rpmostree_set_prefix_error_from_errno (error, errno, "fopen(%s): ", path);
      return NULL;
    }
  return ret;
}

static gboolean
gfflush (FILE         *f,
         GCancellable *cancellable,
         GError      **error)
{
  if (fflush (f) != 0)
    {
      _rpmostree_set_prefix_error_from_errno (error, errno, "fflush: ");
      return FALSE;
    }
  return TRUE;
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
  gboolean ret = FALSE;
  const char *name = kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD ? "passwd" : "group";
  gs_free char *src_path = g_strconcat (gs_file_get_path_cached (rootfs), "/etc/", name, NULL);
  gs_free char *etctmp_path = g_strconcat (gs_file_get_path_cached (rootfs), "/etc/", name, ".tmp", NULL);
  gs_free char *usrdest_path = g_strconcat (gs_file_get_path_cached (rootfs), "/usr/lib/", name, NULL);
  _cleanup_stdio_file_ FILE *src_stream = NULL;
  _cleanup_stdio_file_ FILE *etcdest_stream = NULL;
  _cleanup_stdio_file_ FILE *usrdest_stream = NULL;

  src_stream = gfopen (src_path, "r", cancellable, error);
  if (!src_stream)
    goto out;

  etcdest_stream = gfopen (etctmp_path, "w", cancellable, error);
  if (!etcdest_stream)
    goto out;

  usrdest_stream = gfopen (usrdest_path, "a", cancellable, error);
  if (!usrdest_stream)
    goto out;

  errno = 0;
  while (TRUE)
    {
      struct passwd *pw = NULL;
      struct group *gr = NULL;
      FILE *deststream;
      int r;
      guint32 id;
      const char *name;
      
      if (kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD)
        pw = fgetpwent (src_stream);
      else
        gr = fgetgrent (src_stream);

      if (!(pw || gr))
        {
          if (errno != 0 && errno != ENOENT)
            {
              _rpmostree_set_prefix_error_from_errno (error, errno, "fgetpwent: ");
              goto out;
            }
          else
            break;
        }


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

      if (id == 0)
        deststream = etcdest_stream;
      else
        deststream = usrdest_stream;

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
        {
          _rpmostree_set_prefix_error_from_errno (error, errno, "putpwent: ");
          goto out;
        }
    }

  if (!gfflush (etcdest_stream, cancellable, error))
    goto out;
  if (!gfflush (usrdest_stream, cancellable, error))
    goto out;

  if (rename (etctmp_path, src_path) != 0)
    {
      _rpmostree_set_prefix_error_from_errno (error, errno, "rename(%s, %s): ",
                                              etctmp_path, src_path);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static FILE *
target_etc_filename (GFile         *yumroot,
                     const char    *filename,
                     GCancellable  *cancellable,
                     GError       **error)
{
  FILE *ret = NULL;
  gs_free char *etc_subpath = g_strconcat ("etc/", filename, NULL);
  gs_free char *target_etc =
    g_build_filename (gs_file_get_path_cached (yumroot), etc_subpath, NULL);

  ret = gfopen (target_etc, "w", cancellable, error);
  if (!ret)
    goto out;

 out:
  return ret;
}

static gboolean
_rpmostree_gfile2stdio (GFile         *source,
                        char         **storage_buf,
                        FILE         **ret_src_stream,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gboolean ret = FALSE;
  gsize len;
  FILE *src_stream = NULL;

  /* We read the file into memory using Gio (which talks
   * to libostree), then memopen it, which works with libc.
   */
  if (!g_file_load_contents (source, cancellable,
                             storage_buf, &len, NULL, error))
    goto out;

  if (len == 0)
    goto done;

  src_stream = fmemopen (*storage_buf, len, "r");
  if (!src_stream)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }

 done:
  ret = TRUE;
 out:
  *ret_src_stream = src_stream;
  return ret;
}


static gboolean
concat_entries (FILE    *src_stream,
                FILE    *dest_stream,
                RpmOstreePasswdMigrateKind kind,
                GHashTable *seen_names,
                GError **error)
{
  gboolean ret = FALSE;

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
            {
              _rpmostree_set_prefix_error_from_errno (error, errno, "fgetpwent: ");
              goto out;
            }
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
        {
          _rpmostree_set_prefix_error_from_errno (error, errno, "putpwent: ");
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
concat_passwd_file (GFile           *yumroot,
                    GFile           *previous_commit,
                    RpmOstreePasswdMigrateKind kind,
                    GCancellable    *cancellable,
                    GError         **error)
{
  gboolean ret = FALSE;
  const char *filename = kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD ? "passwd" : "group";
  gs_free char *usretc_subpath = g_strconcat ("usr/etc/", filename, NULL);
  gs_free char *usrlib_subpath = g_strconcat ("usr/lib/", filename, NULL);
  gs_unref_object GFile *orig_etc_content =
    g_file_resolve_relative_path (previous_commit, usretc_subpath);
  gs_unref_object GFile *orig_usrlib_content =
    g_file_resolve_relative_path (previous_commit, usrlib_subpath);
  gs_unref_object GFileOutputStream *out = NULL;
  gs_unref_hashtable GHashTable *seen_names =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  gs_free char *contents = NULL;
  GFile *sources[] = { orig_etc_content, orig_usrlib_content };
  guint i;
  gboolean have_etc, have_usr;
  _cleanup_stdio_file_ FILE *dest_stream = NULL;

  have_etc = g_file_query_exists (orig_etc_content, NULL);
  have_usr = g_file_query_exists (orig_usrlib_content, NULL);

  /* This could actually happen after we transition to
   * systemd-sysusers; we won't have a need for preallocated user data
   * in the tree.
   */
  if (!(have_etc || have_usr))
    {
      ret = TRUE;
      goto out;
    }

  if (!(dest_stream = target_etc_filename (yumroot, filename,
                                           cancellable, error)))
      goto out;

  for (i = 0; i < G_N_ELEMENTS (sources); i++)
    {
      GFile *source = sources[i];
      _cleanup_stdio_file_ FILE *src_stream = NULL;

      if (!_rpmostree_gfile2stdio (source, &contents, &src_stream,
                                   cancellable, error))
        goto out;

      if (!src_stream)
        continue;

      if (!concat_entries (src_stream, dest_stream, kind,
                           seen_names, error))
        goto out;
    }

  if (!gfflush (dest_stream, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
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
  gboolean ret = FALSE;
  const gboolean passwd = kind == RPM_OSTREE_PASSWD_MIGRATE_PASSWD;
  const char *json_conf_name = passwd ? "check-passwd" : "check-groups";
  const char *filebasename   = passwd ? "passwd" : "group";
  JsonObject *chk = NULL;
  const char *chk_type = NULL;
  const char *filename = NULL;
  gs_unref_object GFile *source = NULL;
  gs_free char *contents = NULL;
  gs_unref_hashtable GHashTable *seen_names =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  _cleanup_stdio_file_ FILE *src_stream = NULL;
  _cleanup_stdio_file_ FILE *dest_stream = NULL;

  *out_found = FALSE;
  if (!json_object_has_member (treedata, json_conf_name))
    return TRUE;
  
  chk = json_object_get_object_member (treedata,json_conf_name);
  if (!chk)
    return TRUE;
  
  chk_type = _rpmostree_jsonutil_object_require_string_member (chk, "type",
                                                               error);
  if (!chk_type)
    goto out;

  if (!g_str_equal (chk_type, "file"))
    return TRUE;

  filename = _rpmostree_jsonutil_object_require_string_member (chk,
                                                               "filename",
                                                               error);
  if (!filename)
    goto out;

  source = g_file_resolve_relative_path (treefile_dirpath, filename);
  if (!source)
    goto out;

  /* migrate the check data from the specified file to /etc */
  if (!_rpmostree_gfile2stdio (source, &contents, &src_stream,
                               cancellable, error))
    goto out;

  if (!src_stream)
    return TRUE;

  /* no matter what we've used the data now */
  *out_found = TRUE;

  if (!(dest_stream = target_etc_filename (yumroot, filebasename,
                                           cancellable, error)))
    goto out;

  if (!concat_entries (src_stream, dest_stream, kind, seen_names, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_generate_passwd_from_previous (OstreeRepo      *repo,
                                         GFile           *yumroot,
                                         GFile           *treefile_dirpath,
                                         GFile           *previous_root,
                                         JsonObject      *treedata,
                                         GCancellable    *cancellable,
                                         GError         **error)
{
  gboolean ret = FALSE;
  gboolean found_passwd_data = FALSE;
  gboolean found_groups_data = FALSE;
  gboolean perform_migrate = FALSE;
  gs_unref_object GFile *yumroot_etc = g_file_resolve_relative_path (yumroot, "etc");

  /* Create /etc in the target root; FIXME - should ensure we're using
   * the right permissions from the filesystem RPM.  Doing this right
   * is really hard because filesystem depends on setup which installs
   * the files...
   */
  if (!gs_file_ensure_directory (yumroot_etc, TRUE, cancellable, error))
    goto out;

  if (!_data_from_json (yumroot, treefile_dirpath,
                        treedata, RPM_OSTREE_PASSWD_MIGRATE_PASSWD,
                        &found_passwd_data, cancellable, error))
    goto out;
  perform_migrate = !found_passwd_data;

  if (!previous_root)
    perform_migrate = FALSE;

  if (perform_migrate && !concat_passwd_file (yumroot, previous_root,
                                              RPM_OSTREE_PASSWD_MIGRATE_PASSWD,
                                              cancellable, error))
    goto out;

  if (!_data_from_json (yumroot, treefile_dirpath,
                        treedata, RPM_OSTREE_PASSWD_MIGRATE_GROUP,
                        &found_groups_data, cancellable, error))
    goto out;

  perform_migrate = !found_groups_data;

  if (!previous_root)
    perform_migrate = FALSE;

  if (perform_migrate && !concat_passwd_file (yumroot, previous_root,
                                              RPM_OSTREE_PASSWD_MIGRATE_GROUP,
                                              cancellable, error))
    goto out;

  /* We should error if we are getting passwd data from JSON and group from
   * previous commit, or vice versa, as that'll confuse everyone when it goes
   * wrong. */
  if ( found_passwd_data && !found_groups_data)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Configured to migrate passwd data from JSON, and group data from commit");
      goto out;
    }
  if (!found_passwd_data &&  found_groups_data)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Configured to migrate passwd data from commit, and group data from JSON");
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
