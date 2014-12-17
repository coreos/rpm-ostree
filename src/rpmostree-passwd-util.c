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

#include "rpmostree-compose-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-passwd-util.h"

/* FIXME: */
#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

#include "libgsystem.h"

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

  // Now recurse for dirs.
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
  FILE *mf = NULL;
  GPtrArray *ret = g_ptr_array_new_with_free_func (conv_passwd_ent_free);

  mf = fmemopen ((void *)data, strlen (data), "r");

  while ((ent = fgetpwent (mf)))
    {
      struct conv_passwd_ent *convent = g_new (struct conv_passwd_ent, 1);

      convent->name = g_strdup (ent->pw_name);
      convent->uid  = ent->pw_uid;
      convent->gid  = ent->pw_gid;
      // Want to add anymore, like dir?

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
  FILE *mf = NULL;
  GPtrArray *ret = g_ptr_array_new_with_free_func (conv_group_ent_free);
  
  mf = fmemopen ((void *)data, strlen (data), "r");
  
  while ((ent = fgetgrent (mf)))
    {
      struct conv_group_ent *convent = g_new (struct conv_group_ent, 1);

      convent->name = g_strdup (ent->gr_name);
      convent->gid  = ent->gr_gid;
      // Want to add anymore, like users?

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
      else if (cmp < 0) // Missing value from new passwd
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
      else if (cmp < 0) // Missing value from new group
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
