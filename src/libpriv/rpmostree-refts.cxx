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

#include "rpmostree-refts.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"
#include <rpm/header.h>
#include <rpm/rpmtag.h>
#include <string.h>
#include <stack>

static inline void
cleanup_rpmtdFreeData (rpmtd *tdp)
{
  rpmtd td = *tdp;
  if (td)
    rpmtdFreeData (td);
}
#define _cleanup_rpmtddata_ __attribute__ ((cleanup (cleanup_rpmtdFreeData)))

/*
 * A wrapper for an `rpmts` that supports:
 *
 *  - Reference counting
 *  - Possibly holding a pointer to a tempdir, and cleaning it when unref'd
 */

RpmOstreeRefTs *
rpmostree_refts_new (rpmts ts, GLnxTmpDir *tmpdir)
{
  RpmOstreeRefTs *rts = g_new0 (RpmOstreeRefTs, 1);
  rts->ts = ts;
  rts->refcount = 1;
  if (tmpdir)
    {
      rts->tmpdir = *tmpdir;
      tmpdir->initialized = FALSE; /* Steal ownership */
    }
  return rts;
}

RpmOstreeRefTs *
rpmostree_refts_ref (RpmOstreeRefTs *rts)
{
  g_atomic_int_inc (&rts->refcount);
  return rts;
}

void
rpmostree_refts_unref (RpmOstreeRefTs *rts)
{
  if (!g_atomic_int_dec_and_test (&rts->refcount))
    return;
  rpmtsFree (rts->ts);
  (void)glnx_tmpdir_delete (&rts->tmpdir, NULL, NULL);
  g_free (rts);
}

namespace rpmostreecxx
{
static inline std::pair<std::string_view, std::string_view>
split_filepath (std::string_view path)
{
  std::string_view orig_path = path;

  // Special-case the root path
  if (path == "/")
    return std::pair { path, path };

  // If the path ends with '/', strip it so we
  // properly detect the dirname and basename parts.
  if (!path.empty() && path.back() == '/')
    path.remove_suffix(1);

  auto last_sep = path.find_last_of ('/');
  if (path.empty() || last_sep == std::string_view::npos)
    return std::pair { std::string_view{}, orig_path };
  else if (last_sep > 0)
    return std::pair { path.substr (0, last_sep), path.substr (last_sep + 1) };
  // root dir, or immediate child of root
  else if (last_sep == 0)
    return std::pair { "/", path.substr (1) };
  else
    throw std::runtime_error ("should be unreachable");
}

RpmFileDb::RpmFileDb (const GFile& fs_root_ref)
{
  fs_root = G_FILE (g_object_ref (const_cast<GFile*>(&fs_root_ref)));
}

RpmFileDb::~RpmFileDb ()
{
  if (fs_root != NULL)
    g_object_unref (fs_root);
}

void
RpmFileDb::insert_entry (std::string_view pkg_nevra, std::string_view pkg_path)
{
  auto [dirname_v, basename_v] = split_filepath (pkg_path);
  basename_to_pkginfo[str_cache.get_or_insert (basename_v)].emplace_back (RpmFileDb::PkgFileInfo {
      str_cache.get_or_insert (pkg_nevra),
      str_cache.get_or_insert (dirname_v)
    });
}

rust::Vec<rust::String>
RpmFileDb::find_pkgs_for_file (rust::Str path_rs) const
{
  std::string_view path = std::string_view (path_rs.data (), path_rs.size ());

  auto [dirname_v, basename_v] = split_filepath (path);
  cached_string basename = str_cache.get_or_insert (basename_v);
  
  std::unordered_set<cached_string> containing_pkgs; 
  auto cache_itr = basename_to_pkginfo.find (basename);
  if (cache_itr != basename_to_pkginfo.end ())
    {
      for (const RpmFileDb::PkgFileInfo& entry : cache_itr->second)
        {
          if (fs_paths_are_equivalent (str_cache.get_or_insert (dirname_v), entry))
            containing_pkgs.insert (entry.pkg_nevra);
        }
    }

  rust::Vec<rust::String> results;
  results.reserve (containing_pkgs.size ());
  for (cached_string pkg : containing_pkgs)
    results.emplace_back (str_cache.as_rstring (pkg));
  return results;
}

bool
RpmFileDb::fs_paths_are_equivalent (cached_string dirname, const RpmFileDb::PkgFileInfo& entry) const
{
  // Quick path, strings are the same
  if (dirname == entry.dirname)
    {
      return true;
    }

  // Try to resolve the path from rpmdb against the filesystem to
  // see if it matches the input path (which we assume is valid in the filesystem)
  auto resolved_pkg_path = try_resolve_real_fs_path (entry.dirname);
  return resolved_pkg_path.has_value () && *resolved_pkg_path == dirname;
}

std::optional<cached_string>
RpmFileDb::try_resolve_real_fs_path (cached_string path) const
{
  // First, check the cache to see if we've already resolved this path
  auto fs_cache_itr = fs_resolved_path_cache.find (path);
  if (fs_cache_itr != fs_resolved_path_cache.end ())
      return fs_cache_itr->second;

  std::string_view path_view = str_cache.as_view (path);
  
  // Special-case the root path
  if (path_view == "/")
    {
      fs_resolved_path_cache[path] = path;
      return path;
    }

  // ostree's GFile implementations are finnicky, especially with symlinks,
  // so we'll walk the tree ourselves beginning from the root
  //
  // to do so, we need to break apart our input path into each directory
  std::stack<std::string> path_components;
  while (!path_view.empty () && path_view != "/")
  {
    auto [dirname, basename] = split_filepath (path_view);
    path_components.push (std::string (basename));
    path_view = dirname;
  }

  // Now walk the file system backwards, resolving symlinks as we go
  g_autoptr (GFile) current_path = G_FILE (g_object_ref (fs_root));
  while (!path_components.empty () && current_path != NULL)
    {
      std::string component = path_components.top ();
      path_components.pop ();

      // Try to resolve the next component in the path, exiting if it
      // doesn't exist
      g_autoptr (GFile) child = g_file_get_child (current_path, component.c_str ());
      if (!g_file_query_exists (child, NULL))
        {
          current_path = NULL;
          break;
        }

      // Query for fsinfo so we can check if the path is a symlink
      g_autoptr (GError) error = NULL;
      g_autoptr (GFileInfo) childinfo = g_file_query_info (child, "*", G_FILE_QUERY_INFO_NONE, NULL, &error);
      if (childinfo == NULL)
        {
          g_autofree char* error = g_strdup_printf ("Failed to get file info for '%s'", g_file_peek_path (child));
          throw std::runtime_error (error);
        }

      // If the path is a symlink, remap it
      g_autoptr (GFile) symlink_remapped_path = NULL;
      if (g_file_info_has_attribute (childinfo, "standard::is-symlink") && 
          g_file_info_get_is_symlink (childinfo))
        {
          symlink_remapped_path = g_file_resolve_relative_path (current_path, g_file_info_get_symlink_target (childinfo));
        }

      // Update our tracked current path
      {
        GFile* temp = current_path;
        current_path = symlink_remapped_path != NULL ? 
            G_FILE (g_object_ref (symlink_remapped_path)) : 
            G_FILE (g_object_ref (child));
        g_object_unref (temp);
      }

      // Insert the path into our cache
      cached_string current_path_str = str_cache.get_or_insert (g_file_peek_path (current_path));
      fs_resolved_path_cache[path] = current_path_str;
    }

  if (current_path != NULL)
      return str_cache.get_or_insert (g_file_peek_path (current_path));

  return std::nullopt;
}

RpmTs::RpmTs (RpmOstreeRefTs *ts) { _ts = ts; }

RpmTs::~RpmTs () { rpmostree_refts_unref (_ts); }

std::unique_ptr<PackageMeta>
RpmTs::package_meta (const rust::Str name) const
{
  auto name_c = std::string (name);
  g_auto (rpmdbMatchIterator) mi = rpmtsInitIterator (_ts->ts, RPMDBI_NAME, name_c.c_str (), 0);
  if (mi == NULL)
    {
      g_autofree char *err = g_strdup_printf ("Package not found: %s", name_c.c_str ());
      throw std::runtime_error (err);
    }
  Header h;
  std::optional<rust::String> previous;
  auto retval = std::make_unique<PackageMeta> ();
  while ((h = rpmdbNextIterator (mi)) != NULL)
    {
      auto nevra = rpmostreecxx::header_get_nevra (h);
      if (!previous.has_value ())
        {
          previous = std::move (nevra);
          retval->_size = headerGetNumber (h, RPMTAG_LONGARCHIVESIZE);
          retval->_buildtime = headerGetNumber (h, RPMTAG_BUILDTIME);
          retval->_src_pkg = headerGetString (h, RPMTAG_SOURCERPM);

          // Get the changelogs
          struct rpmtd_s nchanges_date_s;
          _cleanup_rpmtddata_ rpmtd nchanges_date = NULL;
          nchanges_date = &nchanges_date_s;
          headerGet (h, RPMTAG_CHANGELOGTIME, nchanges_date, HEADERGET_MINMEM);
          int ncnum = rpmtdCount (nchanges_date);
          rust::Vec<uint64_t> epochs;
          for (int i = 0; i < ncnum; i++)
            {
              uint64_t nchange_date = 0;
              rpmtdNext (nchanges_date);
              nchange_date = rpmtdGetNumber (nchanges_date);
              epochs.push_back (nchange_date);
            }
          retval->_changelogs = std::move (epochs);
        }
      else
        {
          // TODO: Somehow we get two `libgcc-8.5.0-10.el8.x86_64` in current RHCOS, I don't
          // understand that.
          if (previous != nevra)
            {
              g_autofree char *buf
                  = g_strdup_printf ("Multiple installed '%s' (%s, %s)", name_c.c_str (),
                                     previous.value ().c_str (), nevra.c_str ());
              throw std::runtime_error (buf);
            }
        }
    }
  if (!previous)
    g_assert_not_reached ();
  return retval;
}

std::unique_ptr<RpmFileDb>
RpmTs::build_file_cache_from_rpmdb (const GFile& fs_root) const
{
  std::unique_ptr<RpmFileDb> result = std::make_unique<RpmFileDb> (fs_root);

  g_auto (rpmdbMatchIterator) mi = rpmtsInitIterator (_ts->ts, RPMDBI_PACKAGES, NULL, 0);
  if (mi == NULL)
    throw std::runtime_error ("Failed to init package iterator");

  // Iterate over every path in every package in the database
  // and add them to our cache
  Header h;
  while ((h = rpmdbNextIterator (mi)) != NULL)
    {
      rust::String pkg_nevra = header_get_nevra (h);

      g_auto (rpmfi) fi = rpmfiNew (_ts->ts, h, 0, 0);
      if (fi == NULL)
        throw std::runtime_error ("Failed to get file iterator");

      rpmfiInit (fi, 0);
      while (rpmfiNext (fi) >= 0)
        {
          // Only insert paths in our cache that are marked as installed
          if (RPMFILE_IS_INSTALLED (rpmfiFState (fi)))
            {
              const char* path = rpmfiFN (fi);
              result->insert_entry (
                  std::string_view (pkg_nevra.data (), pkg_nevra.size ()), 
                  path);
            }
        }
    }

  return result;
}

}
