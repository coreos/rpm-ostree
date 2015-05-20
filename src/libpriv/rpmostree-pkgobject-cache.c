/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "rpmostree-rpm-util.h"
#include "rpmostree-refsack.h"
#include "rpmostree-pkgobject-cache.h"

#include <fnmatch.h>
#include <sys/ioctl.h>
#include <hawkey/packageset.h>
#include <rpm/rpmts.h>
#include <rpm/rpmdb.h>
#include <rpm/rpmmacro.h>
#include <libglnx.h>

/**
 * SECTION:rpmostree-pkgobject-cache
 * @title: Cache a mapping from previous tree objects
 *
 * This class is intended to speed up commits for subsequent commits,
 * given a previous commit.  By default, we'll zlib compress + checksum
 * each file which can be slow.  Particularly the zlib compression.
 *
 * This class implements a potential optimization by building up a mapping from
 * `(package version, filename) -> checksum` for the previous commit, then looking
 * in that cache for the new commit.  Thus if a package hasn't changed, we simply
 * return the cached checksum.
 */

static char *
ptrarray_path_join (GPtrArray  *path)
{
  GString *path_buf;

  path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      guint i;
      for (i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];

          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  return g_string_free (path_buf, FALSE);
}

struct RpmOstreePkgObjectCache {
  RpmOstreeRefTs *source_refts;
  RpmOstreeRefTs *target_refts;
  GHashTable *sourcemap;  /* file path -> checksum */
};

RpmOstreePkgObjectCache *
_rpmostree_pkg_object_cache_new (void)
{
  RpmOstreePkgObjectCache *cache = g_new0 (RpmOstreePkgObjectCache, 1);
  cache->sourcemap = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return cache;
}

static char *
cache_key_for_path (const char *path,
                    RpmOstreeRefTs  *refts)
{
  rpmdbMatchIterator mi = NULL;
  char *ret = NULL;
  
  mi = rpmtsInitIterator (refts->ts, RPMDBI_INSTFILENAMES, path, 0);
  if (mi == NULL)
    mi = rpmtsInitIterator (refts->ts, RPMDBI_PROVIDENAME, path, 0);
  
  if (mi != NULL)
    {
      Header h;
      const char *sha1;

      h = rpmdbNextIterator (mi);
      g_assert (h);

      sha1 = headerGetString (h, RPMTAG_SHA1HEADER);
      if (sha1)
        {
          /* RHEL7 RPMs are SHA1...in the future we should handle
           * others, but the md5 -> sha1 transition was *really*
           * painful and I doubt anyone's going to jump to do
           * another.
           */
          ret = g_strconcat (sha1, "-", path, NULL);
        }
    }
  else
    {
      /* Hack to deal with kernel/rpm not handling UsrMove */
      if (g_str_has_prefix (path, "/usr/lib"))
        ret = cache_key_for_path (path + 4, refts);
    }

  rpmdbFreeIterator (mi);
  return ret;
}

static gboolean
pkg_object_cache_load_source (RpmOstreePkgObjectCache *cache,
                              GPtrArray               *path_parts,
                              OstreeRepo              *repo,
                              GFile                   *dirpath,
                              GCancellable            *cancellable,
                              GError                 **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *direnum = NULL;

  direnum = g_file_enumerate_children (dirpath, "standard::name,standard::type",
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
  if (!direnum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;
      GFileType ftype;

      if (!gs_file_enumerator_iterate (direnum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      g_ptr_array_add (path_parts, (char*)g_file_info_get_name (file_info));

      ftype = g_file_info_get_file_type (file_info);
      if (ftype == G_FILE_TYPE_DIRECTORY)
        {

          if (!pkg_object_cache_load_source (cache, path_parts, repo,
                                             child, cancellable, error))
            goto out;

        }
      else if (ftype == G_FILE_TYPE_REGULAR)
        {
          g_autofree char *relpath = ptrarray_path_join (path_parts);
          char *cachekey = cache_key_for_path (relpath, cache->source_refts);

          if (cachekey)
            {
              /* Transfer ownership of cachekey into the cache */
              g_hash_table_insert (cache->sourcemap, cachekey,
                                   g_strdup (ostree_repo_file_get_checksum ((OstreeRepoFile*)child)));
            }
        }

      g_ptr_array_remove_index (path_parts, path_parts->len - 1);
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
_rpmostree_pkg_object_cache_load_source (RpmOstreePkgObjectCache *cache,
                                         OstreeRepo              *repo,
                                         const char              *commit,
                                         GCancellable            *cancellable,
                                         GError                 **error)
{
  gboolean ret = FALSE;
  glnx_unref_object GFile *root = NULL;
  g_autoptr(GPtrArray) path_parts = g_ptr_array_new ();
  
  if (!ostree_repo_read_commit (repo, commit, &root, NULL, cancellable, error))
    goto out;

  if (!rpmostree_get_refts_for_commit (repo, commit, &cache->source_refts,
                                       cancellable, error))
    goto out;

  if (!pkg_object_cache_load_source (cache, path_parts, repo, root, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
_rpmostree_pkg_object_cache_load_target (RpmOstreePkgObjectCache *cache,
                                         int                      dfd,
                                         GCancellable            *cancellable,
                                         GError                 **error)
{
  gboolean ret = FALSE;
  rpmts ts;
  int r;
  g_autofree char *rootpath = NULL;

  /* FIXME - this macro expansion affects all use of librpm in this process.
   * We need an API for this.
   */
  rpmExpand ("%define _dbpath /usr/share/rpm", NULL);

  ts = rpmtsCreate ();
  rpmtsSetVSFlags (ts, _RPMVSF_NODIGESTS | _RPMVSF_NOSIGNATURES);

  rootpath = glnx_fdrel_abspath (dfd, ".");

  r = rpmtsSetRootDir (ts, rootpath);
  g_assert_cmpint (r, ==, 0);

  r = rpmtsOpenDB (ts, O_RDONLY);
  if (r == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to open rpmdb");
      goto out;
    }
  
  cache->target_refts = rpmostree_refts_new (ts, AT_FDCWD, NULL);

  ret = TRUE;
 out:
  return ret;
}

const char *
_rpmostree_pkg_object_cache_query (RpmOstreePkgObjectCache *cache,
                                   const char              *filename)
{
  g_autofree char *cachekey = cache_key_for_path (filename, cache->target_refts);
  if (cachekey)
    return g_hash_table_lookup (cache->sourcemap, cachekey);

  return NULL;
}

void
_rpmostree_pkg_object_cache_free (RpmOstreePkgObjectCache *cache)
{
  g_hash_table_unref (cache->sourcemap);
  g_free (cache);
}
