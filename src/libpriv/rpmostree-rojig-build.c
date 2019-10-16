/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */


#include "config.h"

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include "rpmostree-libarchive-input-stream.h"
#include "rpmostree-unpacker-core.h"
#include "rpmostree-rojig-build.h"
#include "rpmostree-core.h"
#include "rpmostree-rpm-util.h"
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmts.h>
#include <archive.h>
#include <archive_entry.h>

#include <string.h>
#include <stdlib.h>
/* Pair of package, and Set<objid>, which is either a basename, or a full
 * path for non-unique basenames.
 */
typedef struct {
  DnfPackage *pkg;
  GHashTable *objids; /* Set<char *objid> */
} PkgObjid;

static void
pkg_objid_free (PkgObjid *pkgobjid)
{
  g_object_unref (pkgobjid->pkg);
  g_hash_table_unref (pkgobjid->objids);
  g_free (pkgobjid);
}

typedef struct {
  OstreeRepo *repo;
  OstreeRepo *pkgcache_repo;
  RpmOstreeRefSack *rsack;

  guint n_nonunique_objid_basenames;
  guint n_objid_basenames;
  guint duplicate_big_pkgobjects;
  GHashTable *commit_content_objects; /* Set<str Checksum> */
  GHashTable *content_object_to_pkg_objid; /* Map<checksum,PkgObjid> */
  guint n_duplicate_pkg_content_objs;
  guint n_unused_pkg_content_objs;
  GHashTable *objsize_to_object; /* Map<guint32 objsize,checksum> */
} RpmOstreeCommit2RojigContext;

static void
rpm_ostree_commit2rojig_context_free (RpmOstreeCommit2RojigContext *ctx)
{
  g_clear_object (&ctx->repo);
  g_clear_object (&ctx->pkgcache_repo);
  g_clear_pointer (&ctx->rsack, rpmostree_refsack_unref);
  g_clear_pointer (&ctx->commit_content_objects, (GDestroyNotify)g_hash_table_unref);
  g_clear_pointer (&ctx->content_object_to_pkg_objid, (GDestroyNotify)g_hash_table_unref);
  g_clear_pointer (&ctx->objsize_to_object, (GDestroyNotify)g_hash_table_unref);
  g_free (ctx);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeCommit2RojigContext, rpm_ostree_commit2rojig_context_free)

/* Add @objid to the set of objectids for @checksum */
static void
add_objid (GHashTable *object_to_objid, const char *checksum, const char *objid)
{
  GHashTable *objids = g_hash_table_lookup (object_to_objid, checksum);
  if (!objids)
    {
      objids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_hash_table_insert (object_to_objid, g_strdup (checksum), objids);
    }
  g_hash_table_add (objids, g_strdup (objid));
}

/* One the main tricky things we need to handle when building the objidmap is
 * that we want to compress the xattr map some by using basenames if possible.
 * Otherwise we use the full path.
 */
typedef struct {
  DnfPackage *package;
  GHashTable *seen_nonunique_objid; /* Set<char *path> */
  GHashTable *seen_objid_to_path; /* Map<char *objid, char *path> */
  GHashTable *seen_path_to_object; /* Map<char *path, char *checksum> */
  char *tmpfiles_d_path; /* Path to tmpfiles.d, which we skip */
} PkgBuildObjidMap;

static void
pkg_build_objidmap_free (PkgBuildObjidMap *map)
{
  g_clear_pointer (&map->seen_nonunique_objid, (GDestroyNotify)g_hash_table_unref);
  g_clear_pointer (&map->seen_objid_to_path, (GDestroyNotify)g_hash_table_unref);
  g_clear_pointer (&map->seen_path_to_object, (GDestroyNotify)g_hash_table_unref);
  g_free (map->tmpfiles_d_path);
  g_free (map);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PkgBuildObjidMap, pkg_build_objidmap_free)

/* Recursively walk @dir, building a map of object to Set<objid> */
static gboolean
build_objid_map_for_tree (RpmOstreeCommit2RojigContext *self,
                          PkgBuildObjidMap             *build,
                          GHashTable                   *object_to_objid,
                          GFile                        *dir,
                          GCancellable                 *cancellable,
                          GError                      **error)
{
  g_autoptr(GFileEnumerator) direnum =
    g_file_enumerate_children (dir, "standard::name,standard::type",
                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                               cancellable, error);
  if (!direnum)
    return FALSE;
  while (TRUE)
    {
      GFileInfo *info = NULL;
      GFile *child = NULL;
      if (!g_file_enumerator_iterate (direnum, &info, &child, cancellable, error))
        return FALSE;
      if (!info)
        break;
      OstreeRepoFile *repof = (OstreeRepoFile*)child;
      if (!ostree_repo_file_ensure_resolved (repof, NULL))
        g_assert_not_reached ();
      GFileType ftype = g_file_info_get_file_type (info);

      /* Handle directories */
      if (ftype == G_FILE_TYPE_DIRECTORY)
        {
          if (!build_objid_map_for_tree (self, build, object_to_objid, child,
                                         cancellable, error))
            return FALSE;
          continue; /* On to the next */
        }

      g_autofree char *path = g_file_get_path (child);

      /* Handling SELinux labeling for the tmpfiles.d would get very tricky.
       * Currently the rojig unpack path is intentionally "dumb" - we won't
       * synthesize the tmpfiles.d like we do for layering. So punt these into
       * the new object set.
       */
      if (g_str_equal (path, build->tmpfiles_d_path))
        continue;

      const char *checksum = ostree_repo_file_get_checksum (repof);
      const char *bn = glnx_basename (path);
      const gboolean is_known_nonunique = g_hash_table_contains (build->seen_nonunique_objid, bn);
      if (is_known_nonunique)
        {
          add_objid (object_to_objid, checksum, path);
          self->n_nonunique_objid_basenames++;
        }
      else
        {
          const char *existing_path = g_hash_table_lookup (build->seen_objid_to_path, bn);
          if (!existing_path)
            {
              g_hash_table_insert (build->seen_objid_to_path, g_strdup (bn), g_strdup (path));
              g_hash_table_insert (build->seen_path_to_object, g_strdup (path), g_strdup (checksum));
              add_objid (object_to_objid, checksum, bn);
            }
          else
            {
              const char *previous_obj = g_hash_table_lookup (build->seen_path_to_object, existing_path);
              g_assert (previous_obj);
              /* Replace the previous basename with a full path */
              add_objid (object_to_objid, previous_obj, existing_path);
              /* And remove these two hashes which are only needed for transitioning */
              g_hash_table_remove (build->seen_path_to_object, existing_path);
              g_hash_table_remove (build->seen_objid_to_path, bn);
              /* Add to our nonunique set */
              g_hash_table_add (build->seen_nonunique_objid, g_strdup (bn));
              /* And finally our conflicting entry with a full path */
              add_objid (object_to_objid, checksum, path);
              self->n_nonunique_objid_basenames++;
            }
        }
      self->n_objid_basenames++;
    }

  return TRUE;
}

/* For objects bigger than this we'll try to detect identical.
 */
#define BIG_OBJ_SIZE (1024 * 1024)

/* If someone is shipping > 4GB objects...I don't even know.
 * The reason we're doing this is on 32 bit architectures it's
 * a pain to put 64 bit numbers in GHashTable.
 */
static gboolean
query_objsize_assert_32bit (OstreeRepo *repo, const char *checksum,
                            guint32 *out_objsize,
                            GError **error)
{
  guint64 objsize;
  if (!ostree_repo_query_object_storage_size (repo, OSTREE_OBJECT_TYPE_FILE, checksum,
                                              &objsize, NULL, error))
    return FALSE;
  if (objsize > G_MAXUINT32)
    return glnx_throw (error, "Content object '%s' is %" G_GUINT64_FORMAT " bytes, not supported",
                       checksum, objsize);
  *out_objsize = (guint32) objsize;
  return TRUE;
}

static char *
contentonly_hash_for_object (OstreeRepo      *repo,
                             const char      *checksum,
                             GCancellable    *cancellable,
                             GError         **error)
{
  g_autoptr(GInputStream) istream = NULL;
  g_autoptr(GFileInfo) finfo = NULL;
  if (!ostree_repo_load_file (repo, checksum, &istream, &finfo, NULL,
                              cancellable, error))
    return FALSE;

  const guint64 size = g_file_info_get_size (finfo);

  g_autoptr(GChecksum) hasher = g_checksum_new (G_CHECKSUM_SHA256);
  /* See also https://gist.github.com/cgwalters/0df0d15199009664549618c2188581f0
   * and https://github.com/coreutils/coreutils/blob/master/src/ioblksize.h
   * Turns out bigger block size is better; down the line we should use their
   * same heuristics.
   */
  if (size > 0)
    {
      gsize bufsize = MIN (size, 128 * 1024);
      g_autofree char *buf = g_malloc (bufsize);
      gsize bytes_read;
      do
        {
          if (!g_input_stream_read_all (istream, buf, bufsize, &bytes_read, cancellable, error))
            return FALSE;
          g_checksum_update (hasher, (const guint8*)buf, bytes_read);
        }
      while (bytes_read > 0);
    }

  return g_strdup (g_checksum_get_string (hasher));
}

/* Write a single complete new object (in uncompressed object stream form)
 * to the new/ subdir of @tmp_dfd.
 */
static gboolean
write_one_new_object (OstreeRepo             *repo,
                      int                     tmp_dfd,
                      OstreeObjectType        objtype,
                      const char             *checksum,
                      GCancellable           *cancellable,
                      GError                **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Processing new reachable", error);
  g_autoptr(GInputStream) istream = NULL;
  guint64 size;
  if (!ostree_repo_load_object_stream (repo, objtype, checksum,
                                       &istream, &size, cancellable, error))
    return FALSE;

  g_assert (checksum[0] && checksum[1]);
  const char *subdir;
  switch (objtype)
    {
    case OSTREE_OBJECT_TYPE_DIR_META:
      subdir = RPMOSTREE_ROJIG_DIRMETA_DIR;
      break;
    case OSTREE_OBJECT_TYPE_DIR_TREE:
      subdir = RPMOSTREE_ROJIG_DIRTREE_DIR;
      break;
    case OSTREE_OBJECT_TYPE_FILE:
      subdir = RPMOSTREE_ROJIG_NEW_DIR;
      break;
    default:
      g_assert_not_reached ();
    }
  g_autofree char *prefix = g_strdup_printf ("%s/%c%c", subdir, checksum[0], checksum[1]);

  if (!glnx_shutil_mkdir_p_at (tmp_dfd, prefix, 0755, cancellable, error))
    return FALSE;

  g_autofree char *new_obj_path = g_strconcat (prefix, "/", (checksum+2), NULL);
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (tmp_dfd, ".", O_CLOEXEC | O_WRONLY,
                                      &tmpf, error))
    return FALSE;
  g_autoptr(GOutputStream) ostream = g_unix_output_stream_new (tmpf.fd, FALSE);

  if (g_output_stream_splice (ostream, istream, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                              cancellable, error) < 0)
    return FALSE;
  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE, tmp_dfd,
                             new_obj_path, error))
    return FALSE;

  return TRUE;
}

/* Write a set of content-identical objects, with the identical content
 * only written once.  These go in the new-contentident/ subdirectory.
 */
static gboolean
write_content_identical_set (OstreeRepo             *repo,
                             int                     tmp_dfd,
                             guint                   content_ident_idx,
                             GPtrArray              *identicals,
                             GCancellable           *cancellable,
                             GError                **error)
{
  g_assert_cmpint (identicals->len, >, 1);
  GLNX_AUTO_PREFIX_ERROR ("Processing big content-identical", error);
  g_autofree char *subdir = g_strdup_printf ("%s/%u", RPMOSTREE_ROJIG_NEW_CONTENTIDENT_DIR, content_ident_idx);
  if (!glnx_shutil_mkdir_p_at (tmp_dfd, subdir, 0755, cancellable, error))
    return FALSE;

  /* Write metadata for all of the objects as a single variant */
  g_autoptr(GVariantBuilder) builder = g_variant_builder_new (RPMOSTREE_ROJIG_NEW_CONTENTIDENT_VARIANT_FORMAT);
  for (guint i = 0; i < identicals->len; i++)
    {
      const char *checksum = identicals->pdata[i];
      g_autoptr(GFileInfo) finfo = NULL;
      g_autoptr(GVariant) xattrs = NULL;
      if (!ostree_repo_load_file (repo, checksum, NULL, &finfo, &xattrs,
                                  cancellable, error))
        return FALSE;
      g_variant_builder_add (builder, "(suuu@a(ayay))",
                             checksum,
                             GUINT32_TO_BE (g_file_info_get_attribute_uint32 (finfo, "unix::uid")),
                             GUINT32_TO_BE (g_file_info_get_attribute_uint32 (finfo, "unix::gid")),
                             GUINT32_TO_BE (g_file_info_get_attribute_uint32 (finfo, "unix::mode")),
                             xattrs);
    }
  g_autoptr(GVariant) meta = g_variant_ref_sink (g_variant_builder_end (builder));
  g_autofree char *meta_path = g_strconcat (subdir, "/01meta", NULL);
  if (!glnx_file_replace_contents_at (tmp_dfd, meta_path,
                                      g_variant_get_data (meta),
                                      g_variant_get_size (meta),
                                      GLNX_FILE_REPLACE_NODATASYNC,
                                      cancellable, error))
    return FALSE;

  /* Write the content */
  const char *checksum = identicals->pdata[0];
  g_autoptr(GInputStream) istream = NULL;
  if (!ostree_repo_load_file (repo, checksum, &istream,
                              NULL, NULL, cancellable, error))
    return FALSE;
  g_autofree char *content_path = g_strconcat (subdir, "/05content", NULL);
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (tmp_dfd, ".", O_CLOEXEC | O_WRONLY,
                                      &tmpf, error))
    return FALSE;
  g_autoptr(GOutputStream) ostream = g_unix_output_stream_new (tmpf.fd, FALSE);
  if (g_output_stream_splice (ostream, istream, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                              cancellable, error) < 0)
    return FALSE;
  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE, tmp_dfd,
                             content_path, error))
    return FALSE;

  return TRUE;
}

/* Taken from ostree-repo-static-delta-compilation.c */
static guint
bufhash (const void *b, gsize len)
{
  const signed char *p, *e;
  guint32 h = 5381;

  for (p = (signed char *)b, e = (signed char *)b + len; p != e; p++)
    h = (h << 5) + h + *p;

  return h;
}

/* Taken from ostree-repo-static-delta-compilation.c */
static guint
xattr_chunk_hash (const void *vp)
{
  GVariant *v = (GVariant*)vp;
  gsize n = g_variant_n_children (v);
  guint i;
  guint32 h = 5381;

  for (i = 0; i < n; i++)
    {
      const guint8* name;
      const guint8* value_data;
      g_autoptr(GVariant) value = NULL;
      gsize value_len;

      g_variant_get_child (v, i, "(^&ay@ay)",
                           &name, &value);
      value_data = g_variant_get_fixed_array (value, &value_len, 1);

      h += g_str_hash (name);
      h += bufhash (value_data, value_len);
    }

  return h;
}

/* Taken from ostree-repo-static-delta-compilation.c */
static gboolean
xattr_chunk_equals (const void *one, const void *two)
{
  GVariant *v1 = (GVariant*)one;
  GVariant *v2 = (GVariant*)two;
  gsize l1 = g_variant_get_size (v1);
  gsize l2 = g_variant_get_size (v2);

  if (l1 != l2)
    return FALSE;

  if (l1 == 0)
    return l2 == 0;

  return memcmp (g_variant_get_data (v1), g_variant_get_data (v2), l1) == 0;
}

static int
cmp_objidxattrs (gconstpointer ap,
                 gconstpointer bp)
{
  GVariant *a = *((GVariant**)ap);
  GVariant *b = *((GVariant**)bp);
  const char *a_objid;
  g_variant_get_child (a, 0, "&s", &a_objid);
  const char *b_objid;
  g_variant_get_child (b, 0, "&s", &b_objid);
  return strcmp (a_objid, b_objid);
}

/* Walk @pkg, building up a map of content object hash to "objid". */
static gboolean
build_objid_map_for_package (RpmOstreeCommit2RojigContext *self,
                             DnfPackage                   *pkg,
                             GCancellable                 *cancellable,
                             GError                      **error)
{
  const char *errmsg = glnx_strjoina ("build objidmap for ", dnf_package_get_nevra (pkg));
  GLNX_AUTO_PREFIX_ERROR (errmsg, error);
  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
  g_autofree char *pkg_commit = NULL;
  g_autoptr(GFile) commit_root = NULL;

  if (!ostree_repo_read_commit (self->pkgcache_repo, cachebranch, &commit_root, &pkg_commit,
                                cancellable, error))
    return FALSE;

  /* Maps a content object checksum to a set of "objid", which is either
   * a basename (if unique) or a full path.
   */
  g_autoptr(GHashTable) object_to_objid = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                 g_free, (GDestroyNotify)g_hash_table_unref);
  /* Allocate temporary build state (mostly hash tables) just for this call */
  { g_autoptr(PkgBuildObjidMap) build = g_new0 (PkgBuildObjidMap, 1);
    build->package = pkg;
    build->seen_nonunique_objid = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                        g_free, NULL);
    build->seen_objid_to_path = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                      g_free, g_free);
    build->seen_path_to_object = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        g_free, g_free);
    build->tmpfiles_d_path = g_strconcat ("/usr/lib/tmpfiles.d/pkg-",
                                          dnf_package_get_name (pkg), ".conf", NULL);
    if (!build_objid_map_for_tree (self, build, object_to_objid,
                                   commit_root, cancellable, error))
      return FALSE;
  }

  /* Loop over the objects we found in this package */
  GLNX_HASH_TABLE_FOREACH_IT (object_to_objid, it, char *, checksum,
                              GHashTable *, objid_set)
    {
      /* See if this is a "big" object.  If so, we add a mapping from
       * size → checksum, so we can heuristically later try to find
       * "content-identical objects" i.e. they differ only in metadata.
       */
      guint32 objsize = 0;
      if (!query_objsize_assert_32bit (self->pkgcache_repo, checksum, &objsize, error))
        return FALSE;
      if (objsize >= BIG_OBJ_SIZE)
        {
          /* If two big objects that are actually *different* happen
           * to have the same size...eh, not too worried about it right
           * now. It'll just be a missed optimization.  We keep track
           * of how many at least to guide future work.
           */
          if (g_hash_table_replace (self->objsize_to_object, GUINT_TO_POINTER (objsize),
                                    g_strdup (checksum)))
            self->duplicate_big_pkgobjects++;
        }

      if (g_hash_table_contains (self->content_object_to_pkg_objid, checksum))
        {
          /* We already found an instance of this, just add it to our
           * duplicate count as a curiosity.
           */
          self->n_duplicate_pkg_content_objs++;
          g_hash_table_iter_remove (&it);
        }
      else if (!g_hash_table_contains (self->commit_content_objects, checksum))
        {
          /* This happens a lot for Fedora Atomic Host today where we disable
           * documentation. But it will also happen if we modify any files in
           * postprocessing.
           */
          self->n_unused_pkg_content_objs++;
        }
      else
        {
          /* Add object → pkgobjid to the global map */
          g_hash_table_iter_steal (&it);
          PkgObjid *pkgobjid = g_new (PkgObjid, 1);
          pkgobjid->pkg = g_object_ref (pkg);
          pkgobjid->objids = g_steal_pointer (&objid_set);

          g_hash_table_insert (self->content_object_to_pkg_objid, g_steal_pointer (&checksum), pkgobjid);
        }
    }

  return TRUE;
}

/* Converts e.g. x86_64 to x86-64 (which is the current value of the RPM %{_isa}
 * macro). Here's where RPM maintains this currently:
 * https://github.com/rpm-software-management/rpm/blob/d9d47e01146a5d4411691a71916b1030ac7da193/installplatform#L25
 * For now we scrape all the Provides: looking for a `Provides: %{name}(something)`.
 */
static char *
pkg_get_requires_isa (RpmOstreeCommit2RojigContext *self,
                      DnfPackage *pkg,
                      GError    **error)
{
  g_autoptr(DnfReldepList) provides = dnf_package_get_provides (pkg);
  const gint n_provides = dnf_reldep_list_count (provides);
  Pool *pool = dnf_sack_get_pool (self->rsack->sack);
  g_autofree char *provides_prefix = g_strconcat (dnf_package_get_name (pkg), "(", NULL);
  for (int i = 0; i < n_provides; i++)
    {
      DnfReldep *req = dnf_reldep_list_index (provides, i);
      Id reqid = dnf_reldep_get_id (req);
      if (!ISRELDEP (reqid))
        continue;
      Reldep *rdep = GETRELDEP (pool, reqid);
      if (!(rdep->flags & REL_EQ))
        continue;

      const char *name = pool_id2str (pool, rdep->name);

      if (!g_str_has_prefix (name, provides_prefix))
        continue;
      const char *isa_start = name + strlen (provides_prefix);
      const char *endparen = strchr (isa_start, ')');
      if (!endparen)
        continue;

      /* Return the first match.  In theory this would blow up if
       * e.g. a package started doing a Provides: %{name}(awesome) but...
       * why would someone do that?  We can address that if it comes up.
       */
      return g_strndup (isa_start, endparen - isa_start);
    }
  return glnx_null_throw (error, "Missing Provides(%s%%{_isa}) in package: %s",
                          dnf_package_get_name (pkg), dnf_package_get_nevra (pkg));
}

/* Take input spec file and generate a temporary spec file with our metadata
 * inserted.
 */
static char *
generate_spec (RpmOstreeCommit2RojigContext  *self,
               int                            spec_dfd,
               const char                    *spec_path,
               const char                    *ostree_commit_sha256,
               const char                    *rpmostree_inputhash,
               GPtrArray                     *rojig_packages,
               GCancellable                  *cancellable,
               GError                       **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Generating spec", error);
  g_autofree char *spec_contents =
    glnx_file_get_contents_utf8_at (spec_dfd, spec_path, NULL,
                                    cancellable, error);
  if (!spec_contents)
    return NULL;

  /* Look for the magic comment */
  const char *meta = strstr (spec_contents, "\n" RPMOSTREE_ROJIG_SPEC_META_MAGIC);
  if (!meta)
    return glnx_null_throw (error, "Missing magic '%s' in %s", RPMOSTREE_ROJIG_SPEC_META_MAGIC, spec_path);

  /* Generate a replacement in memory */
  g_autoptr(GString) replacement = g_string_new ("");
  g_string_append_len (replacement, spec_contents, meta - spec_contents);
  g_string_append (replacement, "# Generated by rpm-ostree\n");
  g_string_append (replacement, "Provides: " RPMOSTREE_ROJIG_PROVIDE_V5 "\n");
  /* Add provides for the commit hash and inputhash */
  g_string_append (replacement, "Provides: " RPMOSTREE_ROJIG_PROVIDE_COMMIT);
  g_string_append_printf (replacement, "(%s)\n", ostree_commit_sha256);
  if (rpmostree_inputhash)
    {
      g_string_append (replacement, "Provides: " RPMOSTREE_ROJIG_PROVIDE_INPUTHASH);
      g_string_append_printf (replacement, "(%s)\n", rpmostree_inputhash);
    }

  /* Add Requires: on our dependent packages; note this needs to be
   * arch-specific otherwise we may be tripped up by multiarch packages.
   */
  for (guint i = 0; i < rojig_packages->len; i++)
    {
      DnfPackage *pkg = rojig_packages->pdata[i];
      if (g_str_equal (dnf_package_get_arch (pkg), "noarch"))
        {
          g_string_append_printf (replacement, "Requires: %s = %s\n",
                                  dnf_package_get_name (pkg),
                                  dnf_package_get_evr (pkg));
        }
      else
        {
          g_autofree char *isa = pkg_get_requires_isa (self, pkg, error);
          if (!isa)
            return NULL;
          g_string_append_printf (replacement, "Requires: %s(%s) = %s\n",
                                  dnf_package_get_name (pkg), isa,
                                  dnf_package_get_evr (pkg));
        }
    }
  g_string_append (replacement, meta + strlen (RPMOSTREE_ROJIG_SPEC_META_MAGIC) + 1);
  g_string_append (replacement, "# End data generated by rpm-ostree\n");

  g_autofree char *tmppath = g_strdup ("/tmp/rpmostree-rojig-spec.XXXXXX");
  glnx_autofd int fd = g_mkstemp_full (tmppath, O_WRONLY | O_CLOEXEC, 0644);
  if (glnx_loop_write (fd, replacement->str, replacement->len) < 0)
    return glnx_null_throw_errno_prefix (error, "write");

  return g_steal_pointer (&tmppath);
}

static int
compare_pkgs (gconstpointer ap,
              gconstpointer bp)
{
  DnfPackage **a = (gpointer)ap;
  DnfPackage **b = (gpointer)bp;
  return dnf_package_cmp (*a, *b);
}

static gboolean
write_commit2rojig (RpmOstreeCommit2RojigContext *self,
                    const char                   *commit,
                    int                           spec_dfd,
                    const char                   *oirpm_spec,
                    const char                   *outputdir,
                    gboolean                      only_contentdir,
                    GPtrArray                    *pkglist,
                    GHashTable                   *new_reachable_small,
                    GHashTable                   *new_big_content_identical,
                    GCancellable                 *cancellable,
                    GError                      **error)
{
  g_autoptr(GVariant) commit_obj = NULL;
  if (!ostree_repo_load_commit (self->repo, commit, &commit_obj, NULL, error))
    return FALSE;
  g_autoptr(GVariant) commit_inline_meta = g_variant_get_child_value (commit_obj, 0);
  const char *commit_inputhash = NULL;
  (void)g_variant_lookup (commit_inline_meta, "rpmostree.inputhash", "&s", &commit_inputhash);
  g_autoptr(GVariant) commit_detached_meta = NULL;
  if (!ostree_repo_read_commit_detached_metadata (self->repo, commit, &commit_detached_meta,
                                                  cancellable, error))
    return FALSE;

  g_auto(GLnxTmpDir) oirpm_tmpd = { 0, };
  if (!glnx_mkdtemp ("rpmostree-rojig-XXXXXX", 0700, &oirpm_tmpd, error))
    return FALSE;

  /* The commit object and metadata go first, so that the client can do GPG verification
   * early on.
   */
  { g_autofree char *commit_dir = g_strdup_printf ("%s/%c%c", RPMOSTREE_ROJIG_COMMIT_DIR,
                                                   commit[0], commit[1]);
    if (!glnx_shutil_mkdir_p_at (oirpm_tmpd.fd, commit_dir, 0755, cancellable, error))
      return FALSE;
    g_autofree char *commit_path = g_strconcat (commit_dir, "/", commit+2, NULL);
    if (!glnx_file_replace_contents_at (oirpm_tmpd.fd, commit_path,
                                        g_variant_get_data (commit_obj),
                                        g_variant_get_size (commit_obj),
                                        GLNX_FILE_REPLACE_NODATASYNC,
                                        cancellable, error))
      return FALSE;

  }
  { const guint8 *buf = (const guint8*)"";
    size_t buflen = 0;
    g_autofree char *commit_metapath = g_strconcat (RPMOSTREE_ROJIG_COMMIT_DIR, "/meta", NULL);
    if (commit_detached_meta)
      {
        buf = g_variant_get_data (commit_detached_meta);
        buflen = g_variant_get_size (commit_detached_meta);
      }
    if (!glnx_file_replace_contents_at (oirpm_tmpd.fd, commit_metapath,
                                        buf, buflen, GLNX_FILE_REPLACE_NODATASYNC,
                                        cancellable, error))
      return FALSE;
  }

  /* dirtree/dirmeta */
  if (!glnx_shutil_mkdir_p_at (oirpm_tmpd.fd, RPMOSTREE_ROJIG_DIRMETA_DIR, 0755, cancellable, error))
    return FALSE;
  if (!glnx_shutil_mkdir_p_at (oirpm_tmpd.fd, RPMOSTREE_ROJIG_DIRTREE_DIR, 0755, cancellable, error))
    return FALSE;
  /* Traverse the commit again, adding dirtree/dirmeta */
  g_autoptr(GHashTable) commit_reachable = NULL;
  if (!ostree_repo_traverse_commit (self->repo, commit, 0,
                                    &commit_reachable,
                                    cancellable, error))
    return FALSE;
  GLNX_HASH_TABLE_FOREACH_IT (commit_reachable, it, GVariant *, object,
                              GVariant *, also_object)
    {
      OstreeObjectType objtype;
      const char *checksum;
      ostree_object_name_deserialize (object, &checksum, &objtype);
      if (G_IN_SET (objtype, OSTREE_OBJECT_TYPE_DIR_TREE, OSTREE_OBJECT_TYPE_DIR_META))
        {
          if (!write_one_new_object (self->repo, oirpm_tmpd.fd, objtype, checksum,
                                     cancellable, error))
            continue;
        }
      g_hash_table_iter_remove (&it);
    }

  GLNX_HASH_TABLE_FOREACH_IT (new_reachable_small, it, const char *, checksum,
                              void *, unused)
    {
      if (!write_one_new_object (self->repo, oirpm_tmpd.fd,
                                 OSTREE_OBJECT_TYPE_FILE, checksum,
                                 cancellable, error))
        return FALSE;
      g_hash_table_iter_remove (&it);
    }

  /* Process large objects, which may only have 1 reference, in which case they also
   * go under new/, otherwise new-contentident/.
   */
  if (!glnx_shutil_mkdir_p_at (oirpm_tmpd.fd, RPMOSTREE_ROJIG_NEW_CONTENTIDENT_DIR, 0755, cancellable, error))
    return FALSE;
  guint content_ident_idx = 0;
  GLNX_HASH_TABLE_FOREACH_IT (new_big_content_identical, it, const char *, content_checksum,
                              GPtrArray *, identicals)
    {
      g_assert_cmpint (identicals->len, >=, 1);
      if (identicals->len == 1)
        {
          const char *checksum = identicals->pdata[0];
          if (!write_one_new_object (self->repo, oirpm_tmpd.fd,
                                     OSTREE_OBJECT_TYPE_FILE, checksum,
                                     cancellable, error))
            return FALSE;
        }
      else
        {
          if (!write_content_identical_set (self->repo, oirpm_tmpd.fd, content_ident_idx,
                                            identicals, cancellable, error))
            return FALSE;
          content_ident_idx++;
        }
      g_hash_table_iter_remove (&it);
    }

  GLNX_HASH_TABLE_FOREACH_IT (new_big_content_identical, it, const char *, content_checksum,
                              GPtrArray *, identicals)
    {
      g_assert_cmpint (identicals->len, >=, 1);
      if (identicals->len == 1)
        {
          const char *checksum = identicals->pdata[0];
          if (!write_one_new_object (self->repo, oirpm_tmpd.fd,
                                     OSTREE_OBJECT_TYPE_FILE, checksum,
                                     cancellable, error))
            return FALSE;
        }
      else
        {
          if (!write_content_identical_set (self->repo, oirpm_tmpd.fd, content_ident_idx,
                                            identicals, cancellable, error))
            return FALSE;
          content_ident_idx++;
        }
      g_hash_table_iter_remove (&it);
    }

  /* And finally, the xattr data (usually just SELinux labels, the file caps
   * here but *also* in the RPM header; we could optimize that, but it's not
   * really worth it)
   */
  { g_autoptr(GHashTable) xattr_table_hash = g_hash_table_new_full (xattr_chunk_hash, xattr_chunk_equals,
                                                                    (GDestroyNotify)g_variant_unref, NULL);
    if (!glnx_shutil_mkdir_p_at (oirpm_tmpd.fd, RPMOSTREE_ROJIG_XATTRS_DIR, 0755, cancellable, error))
      return FALSE;

    g_autoptr(GHashTable) pkg_to_objidxattrs = g_hash_table_new_full (NULL, NULL, g_object_unref,
                                                                      (GDestroyNotify) g_ptr_array_unref);

    /* First, gather the unique set of xattrs from all pkgobjs */
    g_autoptr(GVariantBuilder) xattr_table_builder =
      g_variant_builder_new (RPMOSTREE_ROJIG_XATTRS_TABLE_VARIANT_FORMAT);
    guint global_xattr_idx = 0;
    GLNX_HASH_TABLE_FOREACH_IT (self->commit_content_objects, it, const char *, checksum,
                                const char *, unused)
      {
        /* Is this content object associated with a package? If not, it was
         * already processed.
         */
        PkgObjid *pkgobjid = g_hash_table_lookup (self->content_object_to_pkg_objid, checksum);
        if (!pkgobjid)
          {
            g_hash_table_iter_remove (&it);
            continue;
          }

        g_autoptr(GVariant) xattrs = NULL;
        if (!ostree_repo_load_file (self->repo, checksum, NULL, NULL,
                                    &xattrs, cancellable, error))
          return FALSE;

        /* No xattrs?  We're done */
        if (g_variant_n_children (xattrs) == 0)
          {
            g_hash_table_iter_remove (&it);
            continue;
          }

        /* Keep track of the unique xattr set */
        void *xattr_idx_p;
        guint this_xattr_idx;
        if (!g_hash_table_lookup_extended (xattr_table_hash, xattrs, NULL, &xattr_idx_p))
          {
            g_variant_builder_add (xattr_table_builder, "@a(ayay)", xattrs);
            g_hash_table_insert (xattr_table_hash, g_steal_pointer (&xattrs),
                                 GUINT_TO_POINTER (global_xattr_idx));
            this_xattr_idx = global_xattr_idx;
            /* Increment this for the next loop */
            global_xattr_idx++;
          }
        else
          {
            this_xattr_idx = GPOINTER_TO_UINT (xattr_idx_p);
          }

        /* Add this to our map of pkg → [objidxattrs] */
        DnfPackage *pkg = pkgobjid->pkg;
        GPtrArray *pkg_objidxattrs = g_hash_table_lookup (pkg_to_objidxattrs, pkg);
        if (!pkg_objidxattrs)
          {
            pkg_objidxattrs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
            g_hash_table_insert (pkg_to_objidxattrs, g_object_ref (pkg), pkg_objidxattrs);
          }

        GLNX_HASH_TABLE_FOREACH (pkgobjid->objids, const char *, objid)
          {
            g_ptr_array_add (pkg_objidxattrs,
                             g_variant_ref_sink (g_variant_new ("(su)", objid, this_xattr_idx)));
          }

        /* We're done with this object data */
        g_hash_table_remove (self->content_object_to_pkg_objid, checksum);
        g_hash_table_iter_remove (&it);
      }

    /* Generate empty entries for the "unused set" - the set of packages
     * that are part of the install, but carry no content objects actually
     * in the tree.  ${foo}-filesystem packages are common examples.  Since
     * v3 the "rojig set" is the same as the "install set".
     */
    for (guint i = 0; i < pkglist->len; i++)
      {
        DnfPackage *pkg = pkglist->pdata[i];
        GPtrArray *pkg_objidxattrs = g_hash_table_lookup (pkg_to_objidxattrs, pkg);
        if (!pkg_objidxattrs)
          {
            pkg_objidxattrs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
            g_hash_table_insert (pkg_to_objidxattrs, g_object_ref (pkg), pkg_objidxattrs);
          }
      }

    g_print ("%u unique xattrs\n", g_hash_table_size (xattr_table_hash));

    /* Write the xattr string table */
    if (!glnx_shutil_mkdir_p_at (oirpm_tmpd.fd, RPMOSTREE_ROJIG_XATTRS_DIR, 0755, cancellable, error))
      return FALSE;
    { g_autoptr(GVariant) xattr_table = g_variant_ref_sink (g_variant_builder_end (xattr_table_builder));
      if (!glnx_file_replace_contents_at (oirpm_tmpd.fd, RPMOSTREE_ROJIG_XATTRS_TABLE,
                                          g_variant_get_data (xattr_table),
                                          g_variant_get_size (xattr_table),
                                          GLNX_FILE_REPLACE_NODATASYNC,
                                          cancellable, error))
        return glnx_prefix_error (error, "Creating xattr table");
    }

    /* Subdirectory for packages */
    if (!glnx_shutil_mkdir_p_at (oirpm_tmpd.fd, RPMOSTREE_ROJIG_XATTRS_PKG_DIR, 0755, cancellable, error))
      return FALSE;

    /* We're done with these maps */
    g_clear_pointer (&self->commit_content_objects, (GDestroyNotify)g_ptr_array_unref);
    g_clear_pointer (&self->content_object_to_pkg_objid, (GDestroyNotify)g_ptr_array_unref);

    /* Now that we have a mapping for each package, sort
     * the package xattr data by objid, and write it to
     * xattrs/${nevra}
     */
    GLNX_HASH_TABLE_FOREACH_IT (pkg_to_objidxattrs, it, DnfPackage*, pkg, GPtrArray *,objidxattrs)
      {
        GLNX_AUTO_PREFIX_ERROR ("Writing xattrs", error);
        const char *nevra = dnf_package_get_nevra (pkg);

        /* Ensure the objid array is sorted so we can bsearch it */
        g_ptr_array_sort (objidxattrs, cmp_objidxattrs);

        /* I am fairly sure that a simple count of the number of objects is
         * sufficient as a cache invalidation mechanism.  Scenarios:
         *
         * - We change the content of a file: Since we do object based imports,
         *   it will be a new object; it'd end up in rojigRPM.
         * - We start wanting an existing pkg object (e.g. docs): Counting works
         * - An object migrates (again add/remove): Counting works
         *
         * And I can't think of a scenario that isn't one of "add,remove,change".
         */
        g_autofree char *cacheid = g_strdup_printf ("%u", objidxattrs->len);

        /* Build up the variant from sorted data */
        g_autoptr(GVariantBuilder) objid_xattr_builder = g_variant_builder_new (RPMOSTREE_ROJIG_XATTRS_PKG_VARIANT_FORMAT);
        g_variant_builder_add (objid_xattr_builder, "s", cacheid);
        g_variant_builder_open (objid_xattr_builder, G_VARIANT_TYPE ("a(su)"));

        for (guint i = 0; i < objidxattrs->len; i++)
          {
            GVariant *objidxattr = objidxattrs->pdata[i];
            g_variant_builder_add (objid_xattr_builder, "@(su)", objidxattr);
          }
        g_variant_builder_close (objid_xattr_builder);
        g_autoptr(GVariant) objid_xattrs_final = g_variant_ref_sink (g_variant_builder_end (objid_xattr_builder));

        g_autofree char *path = g_strconcat (RPMOSTREE_ROJIG_XATTRS_PKG_DIR, "/", nevra, NULL);
        /* The "unused set" will have empty maps for xattrs */
        const guint8 *buf = g_variant_get_data (objid_xattrs_final) ?: "";
        if (!glnx_file_replace_contents_at (oirpm_tmpd.fd, path, buf,
                                            g_variant_get_size (objid_xattrs_final),
                                            GLNX_FILE_REPLACE_NODATASYNC,
                                            cancellable, error))
          return glnx_prefix_error (error, "Writing xattrs to %s", path);

        g_hash_table_iter_remove (&it);
      }
  }

  if (!only_contentdir)
    {
      g_autofree char *tmp_spec =
        generate_spec (self, spec_dfd, oirpm_spec, commit,
                       commit_inputhash, pkglist, cancellable, error);
      if (!tmp_spec)
        return FALSE;

      const char *commit_version;
      if (!g_variant_lookup (commit_inline_meta, OSTREE_COMMIT_META_KEY_VERSION, "&s", &commit_version))
        commit_version = NULL;

      g_autoptr(GPtrArray) rpmbuild_argv = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (rpmbuild_argv, g_strdup ("rpmbuild"));
      g_ptr_array_add (rpmbuild_argv, g_strdup ("-bb"));
      /* We use --build-in-place to avoid having to compress the data again into a
       * Source only to immediately uncompress it.
       */
      g_ptr_array_add (rpmbuild_argv, g_strdup ("--build-in-place"));
      // Taken from https://github.com/cgwalters/homegit/blob/master/bin/rpmbuild-cwd
      const char *rpmbuild_dir_args[] = { "_sourcedir", "_specdir", "_builddir",
                                          "_srcrpmdir", "_rpmdir", };
      for (guint i = 0; i < G_N_ELEMENTS (rpmbuild_dir_args); i++)
        {
          g_ptr_array_add (rpmbuild_argv, g_strdup ("-D"));
          g_ptr_array_add (rpmbuild_argv, g_strdup_printf ("%s %s", rpmbuild_dir_args[i], outputdir));
        }
      g_ptr_array_add (rpmbuild_argv, g_strdup ("-D"));
      g_ptr_array_add (rpmbuild_argv, g_strconcat ("_buildrootdir ", outputdir, "/.build", NULL));
      if (commit_version)
        {
          g_ptr_array_add (rpmbuild_argv, g_strdup ("-D"));
          g_ptr_array_add (rpmbuild_argv, g_strconcat ("ostree_version ", commit_version, NULL));
        }

      g_ptr_array_add (rpmbuild_argv, g_strdup (tmp_spec));
      g_ptr_array_add (rpmbuild_argv, NULL);
      int estatus;
      GLNX_AUTO_PREFIX_ERROR ("Running rpmbuild", error);
      if (!g_spawn_sync (oirpm_tmpd.path, (char**)rpmbuild_argv->pdata, NULL, G_SPAWN_SEARCH_PATH,
                         NULL, NULL, NULL, NULL, &estatus, error))
        return FALSE;
      if (!g_spawn_check_exit_status (estatus, error))
        {
          g_printerr ("Temporary spec retained: %s", tmp_spec);
          return FALSE;
        }

      (void) unlinkat (AT_FDCWD, tmp_spec, 0);
    }
  else
    {
      g_print ("Wrote: %s\n", oirpm_tmpd.path);
      glnx_tmpdir_unset (&oirpm_tmpd);
    }


  return TRUE;
}

/* Entrypoint function for turning a commit into an rojigRPM.
 *
 * The basic prerequisite for this: when doing a compose tree, import the
 * packages, and after import check out the final tree and SELinux relabel the
 * imports so that they're reliably updated (currently depends on some unified
 * core 🌐 work).
 *
 * First, we find the "rojig set" of packages we need; not all packages that
 * live in the tree actually need to be imported; things like `emacs-filesystem`
 * or `rootfiles` today don't actually generate any content objects we use.
 *
 * The biggest "extra data" we need is the SELinux labels for the files in
 * each package.  To simplify things, we generalize this to "all xattrs".
 *
 * Besides that, we need the metadata objects like the OSTree commit and the
 * referenced dirtree/dirmeta objects. Plus the added content objects like the
 * rpmdb, initramfs, etc.
 *
 * One special optimization made is support for detecting "content-identical"
 * added content objects, because right now we have the initramfs 3 times in the
 * tree (due to SELinux labels). While we have 3 copies on disk, we can easily
 * avoid that on the wire.
 *
 * Once we've determined all the needed data, we make a temporary directory, and
 * start writing out files inside it. This temporary directory is then turned
 * into the rojigRPM (what looks like a plain old RPM) by invoking `rpmbuild` using
 * a `.spec` file.
 *
 * The resulting "rojig set" is then that rojigRPM, plus the exact NEVRAs - we also
 * record the repodata checksum (normally sha256), to ensure that we get the
 * *exact* RPMs we require bit-for-bit.
 */
static gboolean
impl_commit2rojig (RpmOstreeCommit2RojigContext *self,
                   const char                   *rev,
                   int                           spec_dfd,
                   const char                   *oirpm_spec,
                   const char                   *outputdir,
                   GCancellable                 *cancellable,
                   GError                      **error)
{
  g_assert_cmpint (*outputdir, ==, '/');
  g_autofree char *commit = NULL;
  g_autoptr(GFile) root = NULL;
  if (!ostree_repo_read_commit (self->repo, rev, &root, &commit, cancellable, error))
    return FALSE;

  g_print ("Finding reachable objects from target %s...\n", commit);
  g_autoptr(GHashTable) commit_reachable = NULL;
  if (!ostree_repo_traverse_commit (self->repo, commit, 0,
                                    &commit_reachable,
                                    cancellable, error))
    return FALSE;
  GLNX_HASH_TABLE_FOREACH_IT (commit_reachable, it, GVariant *, object,
                              GVariant *, also_object)
    {
      OstreeObjectType objtype;
      const char *checksum;
      ostree_object_name_deserialize (object, &checksum, &objtype);
      if (objtype == OSTREE_OBJECT_TYPE_FILE)
        g_hash_table_add (self->commit_content_objects, g_strdup (checksum));
      g_hash_table_iter_remove (&it);
    }
  g_print ("%u content objects\n", g_hash_table_size (self->commit_content_objects));

  g_print ("Finding reachable objects from packages...\n");
  self->rsack = rpmostree_get_refsack_for_commit (self->repo, commit, cancellable, error);
  if (!self->rsack)
    return FALSE;

  hy_autoquery HyQuery hquery = hy_query_create (self->rsack->sack);
  hy_query_filter (hquery, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  g_autoptr(GPtrArray) pkglist = hy_query_run (hquery);
  g_print ("Building object map from %u packages\n", pkglist->len);

  g_assert_cmpint (pkglist->len, >, 0);
  /* Sort now, since writing at least requires it, and it aids predictability */
  g_ptr_array_sort (pkglist, compare_pkgs);

  for (guint i = 0; i < pkglist->len; i++)
    {
      DnfPackage *pkg = pkglist->pdata[i];
      if (!build_objid_map_for_package (self, pkg, cancellable, error))
        return FALSE;
    }

  g_print ("%u content objects in packages\n", g_hash_table_size (self->content_object_to_pkg_objid));
  g_print ("  %u duplicate, %u unused\n",
           self->n_duplicate_pkg_content_objs, self->n_unused_pkg_content_objs);
  g_print ("  %u big sizematches, %u/%u nonunique basenames\n",
           self->duplicate_big_pkgobjects, self->n_nonunique_objid_basenames, self->n_objid_basenames);
  /* These sets track objects which aren't in the packages */
  g_autoptr(GHashTable) new_reachable_big = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) new_reachable_small = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) pkgs_with_content = g_hash_table_new (NULL, NULL);
  guint64 pkg_bytes = 0;
  guint64 oirpm_bytes_small = 0;

  /* Loop over every content object in the final commit, and see whether we
   * found a package that contains that exact object.  We classify new
   * objects as either "big" or "small" - for "big" objects we'll try
   * to heuristically find a content-identical one.
   */
  GLNX_HASH_TABLE_FOREACH (self->commit_content_objects, const char *, checksum)
    {
      guint32 objsize = 0;
      if (!query_objsize_assert_32bit (self->repo, checksum, &objsize, error))
        return FALSE;
      const gboolean is_big = objsize >= BIG_OBJ_SIZE;

      PkgObjid *pkgobjid = g_hash_table_lookup (self->content_object_to_pkg_objid, checksum);
      if (!pkgobjid)
        g_hash_table_add (is_big ? new_reachable_big : new_reachable_small, g_strdup (checksum));
      else
        g_hash_table_add (pkgs_with_content, pkgobjid->pkg);

      if (pkgobjid)
        pkg_bytes += objsize;
      else if (!is_big)
        oirpm_bytes_small += objsize;
      /* We'll account for new big objects later after more analysis */
    }

  g_print ("Found objects in %u/%u packages; new (unpackaged) objects: %u small + %u large\n",
           g_hash_table_size (pkgs_with_content),
           pkglist->len,
           g_hash_table_size (new_reachable_small),
           g_hash_table_size (new_reachable_big));
  if (g_hash_table_size (pkgs_with_content) != pkglist->len)
    {
      g_print ("Packages without content:\n");
      for (guint i = 0; i < pkglist->len; i++)
        {
          DnfPackage *pkg = pkglist->pdata[i];
          if (!g_hash_table_contains (pkgs_with_content, pkg))
            {
              g_autofree char *tmpfiles_d_path = g_strconcat ("usr/lib/tmpfiles.d/pkg-",
                                                              dnf_package_get_name (pkg),
                                                              ".conf", NULL);
              g_autoptr(GFile) tmpfiles_d_f = g_file_resolve_relative_path (root, tmpfiles_d_path);
              const gboolean is_tmpfiles_only = g_file_query_exists (tmpfiles_d_f, cancellable);
              /* I added this while debugging missing tmpfiles.d/pkg-$x.conf
               * objects; it turns out not to trigger currently, but keeping it
               * anyways.
               */
              if (is_tmpfiles_only)
                {
                  g_hash_table_add (pkgs_with_content, pkg);
                  g_print ("  %s (tmpfiles only)\n", dnf_package_get_nevra (pkg));
                }
              else
                {
                  g_autofree char *pkgsize_str = g_format_size (dnf_package_get_size (pkg));
                  g_print ("  %s (%s)\n", dnf_package_get_nevra (pkg), pkgsize_str);
                }
            }
        }
      g_print ("\n");
    }

#if 0
  /* Maps a new big object hash to an object from a package */
  g_autoptr(GHashTable) new_big_pkgidentical = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
#endif
  g_print ("Examining large objects more closely for content-identical versions...\n");
  /* Maps a new big object hash to a set of duplicates; yes this happens
   * unfortunately for the initramfs right now due to SELinux labeling.
   */
  g_autoptr(GHashTable) new_big_content_identical = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                           g_free, (GDestroyNotify)g_ptr_array_unref);

  guint64 oirpm_bytes_big = 0;
  GLNX_HASH_TABLE_FOREACH_IT (new_reachable_big, it, const char *, checksum,
                              void *, unused)
    {
      guint32 objsize = 0;
      if (!query_objsize_assert_32bit (self->repo, checksum, &objsize, error))
        return FALSE;
      g_assert_cmpint (objsize, >=, BIG_OBJ_SIZE);

      g_autofree char *obj_contenthash =
        contentonly_hash_for_object (self->repo, checksum, cancellable, error);
      if (!obj_contenthash)
        return FALSE;
      g_autofree char *objsize_formatted = g_format_size (objsize);

      /* This is complex to implement; it would be useful for the grub2-efi data
       * but in the end we should avoid having content-identical objects in the
       * OS data anyways.
       */
#if 0
      const char *probable_source = g_hash_table_lookup (self->objsize_to_object, GUINT_TO_POINTER (objsize));

      if (probable_source)
        {
          g_autofree char *probable_source_contenthash =
            contentonly_hash_for_object (self->pkgcache_repo, probable_source, cancellable, error);
          if (!probable_source_contenthash)
            return FALSE;
          if (g_str_equal (obj_contenthash, probable_source_contenthash))
            {
              g_print ("%s %s (pkg content hash hit)\n", checksum, objsize_formatted);
              g_hash_table_replace (new_big_pkgidentical, g_strdup (checksum), g_strdup (probable_source));
              g_hash_table_iter_remove (&it);

              pkg_bytes += objsize;
              /* We found a package content hash hit, loop to next */
              continue;
            }
        }
#endif

      /* OK, see if it duplicates another *new* object */
      GPtrArray *identicals = g_hash_table_lookup (new_big_content_identical, obj_contenthash);
      if (!identicals)
        {
          identicals = g_ptr_array_new_with_free_func (g_free);
          g_hash_table_insert (new_big_content_identical, g_strdup (obj_contenthash), identicals);
          g_print ("%s %s (new, objhash %s)\n", checksum, objsize_formatted, obj_contenthash);
          oirpm_bytes_big += objsize;
        }
      else
        {
          g_print ("%s (content identical with %u objects)\n", checksum, identicals->len);
        }
      g_ptr_array_add (identicals, g_strdup (checksum));
    }

  { g_autofree char *pkg_bytes_formatted = g_format_size (pkg_bytes);
    g_autofree char *oirpm_bytes_formatted_small = g_format_size (oirpm_bytes_small);
    g_autofree char *oirpm_bytes_formatted_big = g_format_size (oirpm_bytes_big);
    g_print ("pkg content size: %s\n", pkg_bytes_formatted);
    g_print ("oirpm content size (small objs): %s\n", oirpm_bytes_formatted_small);
    g_print ("oirpm content size (big objs): %s\n", oirpm_bytes_formatted_big);
  }

  /* Hardcode FALSE for opt_only_contentdir for now */
  if (!write_commit2rojig (self, commit, spec_dfd, oirpm_spec, outputdir, FALSE, pkglist,
                           new_reachable_small, new_big_content_identical,
                           cancellable, error))
    return FALSE;

  return TRUE;
}

/* See comment for impl_commit2rojig() */
gboolean
rpmostree_commit2rojig (OstreeRepo   *repo,
                        OstreeRepo   *pkgcache_repo,
                        const char   *commit,
                        int           spec_dfd,
                        const char   *spec,
                        const char   *outputdir,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(RpmOstreeCommit2RojigContext) self = g_new0 (RpmOstreeCommit2RojigContext, 1);

  self->repo = g_object_ref (repo);
  self->pkgcache_repo = g_object_ref (pkgcache_repo);

  self->commit_content_objects = g_hash_table_new_full (g_str_hash, g_str_equal, (GDestroyNotify)g_free, NULL);
  self->content_object_to_pkg_objid = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                             (GDestroyNotify)g_free,(GDestroyNotify)pkg_objid_free);
  self->objsize_to_object = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_free);

  return impl_commit2rojig (self, commit, spec_dfd, spec, outputdir, cancellable, error);
}
