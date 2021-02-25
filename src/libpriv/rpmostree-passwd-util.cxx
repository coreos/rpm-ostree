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
#include "rpmostree-rust.h"

#include "libglnx.h"

static void
conv_passwd_ent_free (void *vptr)
{
  auto ptr = static_cast<struct conv_passwd_ent *>(vptr);

  g_free (ptr->name);
  g_free (ptr->pw_gecos);
  g_free (ptr->pw_dir);
  g_free (ptr->pw_shell);
  g_free (ptr);
}

GPtrArray *
rpmostree_passwd_data2passwdents (const char *data)
{
  struct passwd *ent = NULL;
  GPtrArray *ret = g_ptr_array_new_with_free_func (conv_passwd_ent_free);

  g_assert (data != NULL);

  g_autoptr(FILE) mf = fmemopen ((void *)data, strlen (data), "r");

  while ((ent = fgetpwent (mf)))
    {
      struct conv_passwd_ent *convent = g_new (struct conv_passwd_ent, 1);

      convent->name = g_strdup (ent->pw_name);
      convent->uid  = ent->pw_uid;
      convent->gid  = ent->pw_gid;
      convent->pw_gecos = g_strdup (ent->pw_gecos);
      convent->pw_dir = g_strdup (ent->pw_dir);
      convent->pw_shell = g_strdup (ent->pw_shell);
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
  auto ptr = static_cast<struct conv_group_ent *>(vptr);

  g_free (ptr->name);
  g_free (ptr);
}

GPtrArray *
rpmostree_passwd_data2groupents (const char *data)
{
  g_assert (data != NULL);

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
                               int              rootfs_fd,
                               rpmostreecxx::Treefile &treefile_rs,
                               JsonObject      *treedata,
                               const char      *previous_commit,
                               GCancellable    *cancellable,
                               GError         **error)
{
  const char *chk_type = "previous";
  const char *commit_filepath = passwd ? "usr/lib/passwd" : "usr/lib/group";
  const char *json_conf_name  = passwd ? "check-passwd" : "check-groups";
  const char *json_conf_ign   = passwd ? "ignore-removed-users" : "ignore-removed-groups";
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
      else if (g_str_equal (chk_type, "previous"))
        ; /* Handled below */
      else if (g_str_equal (chk_type, "file"))
        ; /* Handled below */
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
              auto name = static_cast<const char *>(iter->data);
              JsonNode *val = json_object_get_member (ents_obj, name);
              JsonNodeType child_type = json_node_get_node_type (val);
              gint64 uid = 0;
              gint64 gid = 0;

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

              struct conv_passwd_ent *convent = g_new (struct conv_passwd_ent, 1);
              convent->name = g_strdup (name);
              convent->uid  = uid;
              convent->gid  = gid;
              g_ptr_array_add (old_ents, convent);
            }
            else
            {
              auto name = static_cast<const char *>(iter->data);
              gint64 gid = 0;

              if (!_rpmostree_jsonutil_object_require_int_member (ents_obj, name, &gid, error))
                return FALSE;

              auto convent = static_cast<struct conv_group_ent *>(g_new (struct conv_group_ent, 1));
              convent->name = g_strdup (name);
              convent->gid  = gid;
              g_ptr_array_add (old_ents, convent);
            }
        }
      else
        return glnx_throw (error, "Invalid %s type '%s'", json_conf_name, chk_type);
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
      int fd = passwd ? treefile_rs.get_passwd_fd() :
        treefile_rs.get_group_fd();
      old_contents = glnx_fd_readall_utf8 (fd, NULL, cancellable, error);
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
    glnx_file_get_contents_utf8_at (rootfs_fd, commit_filepath, NULL,
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
      auto odata = static_cast<struct conv_passwd_ent *>(old_ents->pdata[oiter]);
      auto ndata = static_cast<struct conv_passwd_ent *>(new_ents->pdata[niter]);
      int cmp = g_strcmp0 (odata->name, ndata->name);

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
          if (ignore_all_removed ||
              rpmostree_str_ptrarray_contains (ignore_removed_ents, odata->name))
            {
              g_print ("Ignored user missing from new passwd file: %s\n",
                       odata->name);
            }
          else
            {
              auto found_matching_uid = rpmostreecxx::dir_contains_uid(rootfs_fd, odata->uid);
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
      auto odata = static_cast<struct conv_group_ent *>(old_ents->pdata[oiter]);
      auto ndata = static_cast<struct conv_group_ent *>(new_ents->pdata[niter]);
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
              auto found_matching_gid = rpmostreecxx::dir_contains_gid(rootfs_fd, odata->gid);
              if (found_matching_gid)
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
          auto odata = static_cast<struct conv_passwd_ent *>(old_ents->pdata[oiter]);

          return glnx_throw (error, "User missing from new passwd file: %s", odata->name);
        }

      while (niter < new_ents->len)
        {
          auto ndata = static_cast<struct conv_passwd_ent *>(new_ents->pdata[niter]);

          g_print ("New passwd entry: %s\n", ndata->name);
          ++niter;
        }
    }
  else
    {
      if (oiter < old_ents->len)
        {
          auto odata = static_cast<struct conv_group_ent *>(old_ents->pdata[oiter]);

          return glnx_throw (error, "Group missing from new group file: %s", odata->name);
        }

      while (niter < new_ents->len)
        {
          auto ndata = static_cast<struct conv_group_ent *>(new_ents->pdata[niter]);

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
                        int              rootfs_fd,
                        rpmostreecxx::Treefile &treefile_rs,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error)
{
  return rpmostree_check_passwd_groups (TRUE, repo, rootfs_fd, treefile_rs,
                                        treedata, previous_commit,
                                        cancellable, error);
}

/* See "man 5 group" We just need to make sure the name and gid match,
   and that none are missing. Don't care about users.
 */
gboolean
rpmostree_check_groups (OstreeRepo      *repo,
                        int              rootfs_fd,
                        rpmostreecxx::Treefile &treefile_rs,
                        JsonObject      *treedata,
                        const char      *previous_commit,
                        GCancellable    *cancellable,
                        GError         **error)
{
  return rpmostree_check_passwd_groups (FALSE, repo, rootfs_fd, treefile_rs,
                                        treedata, previous_commit,
                                        cancellable, error);
}
