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

static char *
load_file_direct_or_rev (OstreeRepo      *repo,
                         const char      *direct_or_rev,
                         const char      *path,
                         GCancellable    *cancellable,
                         GError         **error)
{
  gs_unref_object GFile *root = NULL;
  gs_unref_object GFile *fpathd = g_file_new_for_path (direct_or_rev);
  gs_unref_object GFile *fpathc = NULL;
  GError *tmp_error = NULL;
  char *ret = NULL;

  ret = gs_file_load_contents_utf8 (fpathd, cancellable, &tmp_error);
  if (ret)
    goto out;

  if (!path)
    {
      g_propagate_error (error, tmp_error);
      goto out;
    }
  g_clear_error (&tmp_error);

  if (!ostree_repo_read_commit (repo, direct_or_rev, &root, NULL, NULL, error))
    goto out;

  fpathc = g_file_resolve_relative_path (root, path);
  ret = gs_file_load_contents_utf8 (fpathc, cancellable, error);

 out:
  return ret;
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

/* See "man 5 passwd" We just make sure the name and uid/gid match,
   and that none are missing. don't care about GECOS/dir/shell.
*/
gboolean
rpmostree_check_passwd (OstreeRepo      *repo,
                        const char      *direct_or_rev,
                        GFile           *yumroot,
                        JsonObject      *treedata,
                        GCancellable    *cancellable,
                        GError         **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *new_path = g_file_resolve_relative_path (yumroot, "usr/lib/passwd");
  gs_unref_ptrarray GPtrArray *ignore_removed_users = NULL;
  gboolean ignore_all_removed = FALSE;
  gs_free char *old_contents = NULL;
  gs_free char *new_contents = NULL;
  gs_unref_ptrarray GPtrArray *old_ents = NULL;
  gs_unref_ptrarray GPtrArray *new_ents = NULL;
  unsigned int oiter = 0;
  unsigned int niter = 0;

  old_contents = load_file_direct_or_rev (repo,
                                          direct_or_rev, "usr/lib/passwd",
                                          cancellable, error);
  if (!old_contents)
    goto out;

  new_contents = gs_file_load_contents_utf8 (new_path, cancellable, error);
  if (!new_contents)
      goto out;

  old_ents = data2passwdents (old_contents);
  g_ptr_array_sort (old_ents, compare_passwd_ents);
  
  if (json_object_has_member (treedata, "ignore-removed-users"))
    {
      ignore_removed_users = g_ptr_array_new ();
      if (!_rpmostree_jsonutil_append_string_array_to (treedata, "ignore-removed-users", ignore_removed_users, error))
        goto out;
    }
  ignore_all_removed = ptrarray_contains_str (ignore_removed_users, "*");

  new_ents = data2passwdents (new_contents);
  g_ptr_array_sort (new_ents, compare_passwd_ents);

  while ((oiter < old_ents->len) && (niter < new_ents->len))
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
              ptrarray_contains_str (ignore_removed_users, odata->name))
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
            }
              
          ++oiter;
        }
      else
        {
          g_print ("New passwd entry: %s\n", ndata->name);
          ++niter;
        }
    }

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

  ret = TRUE;
 out:
  return ret;
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

/* See "man 5 group" We just need to make sure the name and gid match,
   and that none are missing. Don't care about users.
 */
gboolean
rpmostree_check_groups (OstreeRepo      *repo,
                        const char      *direct_or_rev,
                        GFile           *yumroot,
                        JsonObject      *treedata,
                        GCancellable    *cancellable,
                        GError         **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *new_path = g_file_resolve_relative_path (yumroot, "usr/lib/group");
  gs_unref_ptrarray GPtrArray *ignore_removed_groups = NULL;
  gboolean ignore_all_removed = FALSE;
  gs_free char *old_contents = NULL;
  gs_free char *new_contents = NULL;
  gs_unref_ptrarray GPtrArray *old_ents = NULL;
  gs_unref_ptrarray GPtrArray *new_ents = NULL;
  unsigned int oiter = 0;
  unsigned int niter = 0;

  old_contents = load_file_direct_or_rev (repo,
                                          direct_or_rev, "usr/lib/group",
                                          cancellable, error);
  if (!old_contents)
    goto out;

  new_contents = gs_file_load_contents_utf8 (new_path, cancellable, error);
  if (!new_contents)
    goto out;

  old_ents = data2groupents (old_contents);
  g_ptr_array_sort (old_ents, compare_group_ents);
  
  if (json_object_has_member (treedata, "ignore-removed-groups"))
    {
      ignore_removed_groups = g_ptr_array_new ();
      if (!_rpmostree_jsonutil_append_string_array_to (treedata, "ignore-removed-groups", ignore_removed_groups, error))
        goto out;
    }
  ignore_all_removed = ptrarray_contains_str (ignore_removed_groups, "*");

  new_ents = data2groupents (new_contents);
  g_ptr_array_sort (new_ents, compare_group_ents);

  while ((oiter < old_ents->len) && (niter < new_ents->len))
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
              ptrarray_contains_str (ignore_removed_groups, odata->name))
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
            }
          
          ++oiter;
        }
      else
        {
          g_print ("New group entry: %s\n", ndata->name);
          ++niter;
        }
    }

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

  ret = TRUE;
 out:
  return ret;
}
