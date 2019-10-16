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
#include "rpmostree-libarchive-input-stream.h"
#include "rpmostree-unpacker-core.h"
#include "rpmostree-rojig-assembler.h"
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

typedef enum {
  STATE_COMMIT,
  STATE_DIRMETA,
  STATE_DIRTREE,
  STATE_NEW_CONTENTIDENT,
  STATE_NEW,
  STATE_XATTRS_TABLE,
  STATE_XATTRS_PKG,
} RojigAssemblerState;

static gboolean
throw_libarchive_error (GError      **error,
                        struct archive *a)
{
  return glnx_throw (error, "%s", archive_error_string (a));
}

typedef GObjectClass RpmOstreeRojigAssemblerClass;

struct RpmOstreeRojigAssembler
{
  GObject parent_instance;
  RojigAssemblerState state;
  DnfPackage *pkg;
  GVariant *commit;
  GVariant *meta;
  char *checksum;
  GVariant *xattrs_table;
  struct archive *archive;
  struct archive_entry *next_entry;
  int fd;
};

G_DEFINE_TYPE(RpmOstreeRojigAssembler, rpmostree_rojig_assembler, G_TYPE_OBJECT)

static void
rpmostree_rojig_assembler_finalize (GObject *object)
{
  RpmOstreeRojigAssembler *self = (RpmOstreeRojigAssembler*)object;
  if (self->archive)
    archive_read_free (self->archive);
  g_clear_pointer (&self->commit, (GDestroyNotify)g_variant_unref);
  g_clear_pointer (&self->meta, (GDestroyNotify)g_variant_unref);
  g_free (self->checksum);
  g_clear_object (&self->pkg);
  g_clear_pointer (&self->xattrs_table, (GDestroyNotify)g_variant_unref);
  glnx_close_fd (&self->fd);

  G_OBJECT_CLASS (rpmostree_rojig_assembler_parent_class)->finalize (object);
}

static void
rpmostree_rojig_assembler_class_init (RpmOstreeRojigAssemblerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = rpmostree_rojig_assembler_finalize;
}

static void
rpmostree_rojig_assembler_init (RpmOstreeRojigAssembler *self)
{
  self->fd = -1;
}

/*
 * rpmostree_rojig_assembler_new_take_fd:
 * @fd: Fd, ownership is taken
 * @pkg: (optional): Package reference, used for metadata
 * @error: error
 *
 * Create a new unpacker instance.  The @pkg argument, if
 * specified, will be inspected and metadata such as the
 * origin repo will be added to the final commit.
 */
RpmOstreeRojigAssembler *
rpmostree_rojig_assembler_new_take_fd (int *fd,
                             DnfPackage *pkg,
                             GError **error)
{
  glnx_fd_close int owned_fd = glnx_steal_fd (fd);

  struct archive *archive = rpmostree_unpack_rpm2cpio (owned_fd, error);
  if (archive == NULL)
    return NULL;

  RpmOstreeRojigAssembler *ret = g_object_new (RPMOSTREE_TYPE_ROJIG_ASSEMBLER, NULL);
  ret->archive = g_steal_pointer (&archive);
  ret->pkg = pkg ? g_object_ref (pkg) : NULL;
  ret->fd = glnx_steal_fd (&owned_fd);

  return g_steal_pointer (&ret);
}

static GVariant *
rojig_read_variant (const GVariantType   *vtype,
                    struct archive       *a,
                    struct archive_entry *entry,
                    GCancellable        *cancellable,
                    GError             **error)
{
  const char *path = archive_entry_pathname (entry);
  const struct stat *stbuf = archive_entry_stat (entry);
  if (!S_ISREG (stbuf->st_mode))
    return glnx_null_throw (error, "Expected regular file for entry: %s", path);
  if (!rpmostree_check_size_within_limit (stbuf->st_size, OSTREE_MAX_METADATA_SIZE,
                                          path, error))
    return NULL;
  g_assert_cmpint (stbuf->st_size, >=, 0);
  const size_t total = stbuf->st_size;
  g_autofree guint8* buf = g_malloc (total);
  size_t bytes_read = 0;
  while (bytes_read < total)
    {
      ssize_t r = archive_read_data (a, buf + bytes_read, total - bytes_read);
      if (r < 0)
        return throw_libarchive_error (error, a), NULL;
      if (r == 0)
        break;
      bytes_read += r;
    }
  g_assert_cmpint (bytes_read, ==, total)
;
  /* Need to take ownership once now, then pass it as a parameter twice */
  guint8* buf_owned = g_steal_pointer (&buf);
  return g_variant_new_from_data (vtype, buf_owned, bytes_read, FALSE, g_free, buf_owned);
}

/* Remove leading prefix */
static const char *
peel_entry_pathname (struct archive_entry *entry,
                     GError    **error)
{
  const char *pathname = archive_entry_pathname (entry);
  static const char prefix[] = "./usr/lib/ostree-jigdo/";
  if (!g_str_has_prefix (pathname, prefix))
    return glnx_null_throw (error, "Entry does not have prefix '%s': %s", prefix, pathname);
  pathname += strlen (prefix);
  const char *nextslash = strchr (pathname, '/');
  if (!nextslash)
    return glnx_null_throw (error, "Missing subdir in %s", pathname);
  return nextslash+1;
}

static gboolean
rojig_next_entry (RpmOstreeRojigAssembler     *self,
                  gboolean           *out_eof,
                  struct archive_entry **out_entry,
                  GCancellable      *cancellable,
                  GError           **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  *out_eof = FALSE;

  if (self->next_entry)
    {
      *out_entry = g_steal_pointer (&self->next_entry);
      return TRUE; /* 🔚 Early return */
    }

  /* Loop, skipping non-regular files */
  struct archive_entry *entry = NULL;
  while (TRUE)
    {
      int r = archive_read_next_header (self->archive, &entry);
      if (r == ARCHIVE_EOF)
        {
          *out_eof = TRUE;
          return TRUE; /* 🔚 Early return */
        }
      if (r != ARCHIVE_OK)
        return throw_libarchive_error (error, self->archive);

      const struct stat *stbuf = archive_entry_stat (entry);
      /* We only have regular files, ignore intermediate dirs */
      if (!S_ISREG (stbuf->st_mode))
        continue;

      /* Otherwise we're done */
      break;
    }

  *out_eof = FALSE;
  *out_entry = entry; /* Owned by archive */
  return TRUE;
}

static struct archive_entry *
rojig_require_next_entry (RpmOstreeRojigAssembler    *self,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean eof = FALSE;
  struct archive_entry *entry = NULL;
  if (!rojig_next_entry (self, &eof, &entry, cancellable, error))
    return FALSE;
  if (eof)
    return glnx_null_throw (error, "Unexpected end of archive");
  return entry;
}

static char *
parse_checksum_from_pathname (const char *pathname,
                              GError    **error)
{
  /* We have an extra / */
  if (strlen (pathname) != OSTREE_SHA256_STRING_LEN + 1)
    return glnx_null_throw (error, "Invalid checksum path: %s", pathname);
  g_autoptr(GString) buf = g_string_new ("");
  g_string_append_len (buf, pathname, 2);
  g_string_append (buf, pathname+3);
  return g_string_free (g_steal_pointer (&buf), FALSE);
}

/* First step: read metadata: the commit object and its metadata, suitable for
 * GPG verification.
 */
gboolean
rpmostree_rojig_assembler_read_meta (RpmOstreeRojigAssembler    *self,
                           char             **out_checksum,
                           GVariant         **out_commit,
                           GVariant         **out_detached_meta,
                           GCancellable      *cancellable,
                           GError           **error)
{
  g_assert_cmpint (self->state, ==, STATE_COMMIT);
  struct archive_entry *entry = rojig_require_next_entry (self, cancellable, error);
  if (!entry)
    return FALSE;
  const char *entry_path = peel_entry_pathname (entry, error);
  if (!entry_path)
    return FALSE;
  if (!g_str_has_prefix (entry_path, RPMOSTREE_ROJIG_COMMIT_DIR "/"))
    return glnx_throw (error, "Unexpected entry: %s", entry_path);
  entry_path += strlen (RPMOSTREE_ROJIG_COMMIT_DIR "/");

  g_autofree char *checksum = parse_checksum_from_pathname (entry_path, error);
  if (!checksum)
    return FALSE;

  g_autoptr(GVariant) commit = rojig_read_variant (OSTREE_COMMIT_GVARIANT_FORMAT,
                                                   self->archive, entry, cancellable, error);
  g_autoptr(GVariant) meta = NULL;

  g_autoptr(GChecksum) hasher = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (hasher, g_variant_get_data (commit), g_variant_get_size (commit));
  const char *actual_checksum = g_checksum_get_string (hasher);

  if (!g_str_equal (checksum, actual_checksum))
    return glnx_throw (error, "Checksum mismatch; described='%s' actual='%s'",
                       checksum, actual_checksum);

  entry = rojig_require_next_entry (self, cancellable, error);
  entry_path = peel_entry_pathname (entry, error);
  if (!entry_path)
    return FALSE;
  if (g_str_equal (entry_path, RPMOSTREE_ROJIG_COMMIT_DIR "/meta"))
    {
      meta = rojig_read_variant (G_VARIANT_TYPE ("a{sv}"), self->archive, entry,
                                 cancellable, error);
      if (!meta)
        return FALSE;
    }
  else
    {
      self->next_entry = entry; /* Stash for next call */
    }

  self->state = STATE_DIRMETA;
  self->checksum = g_strdup (actual_checksum);
  self->commit = g_variant_ref (commit);
  self->meta = meta ? g_variant_ref (meta) : NULL;
  *out_checksum = g_steal_pointer (&checksum);
  *out_commit = g_steal_pointer (&commit);
  *out_detached_meta = g_steal_pointer (&meta);
  return TRUE;
}

static gboolean
process_contentident (RpmOstreeRojigAssembler    *self,
                      OstreeRepo        *repo,
                      struct archive_entry *entry,
                      const char        *meta_pathname,
                      GCancellable      *cancellable,
                      GError           **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Processing content-identical", error);
  /* Read the metadata variant, which has an array of checksum, metadata
   * for regfile objects that have identical content.
   */
  if (!g_str_has_suffix (meta_pathname, "/01meta"))
    return glnx_throw (error, "Malformed contentident: %s", meta_pathname);
  const char *contentident_id_start = meta_pathname + strlen (RPMOSTREE_ROJIG_NEW_CONTENTIDENT_DIR "/");
  const char *slash = strchr (contentident_id_start, '/');
  if (!slash)
    return glnx_throw (error, "Malformed contentident: %s", meta_pathname);
  // g_autofree char *contentident_id_str = g_strndup (contentident_id_start, slash - contentident_id_start);

  g_autoptr(GVariant) meta = rojig_read_variant (RPMOSTREE_ROJIG_NEW_CONTENTIDENT_VARIANT_FORMAT,
                                                 self->archive, entry,
                                                 cancellable, error);


  /* Read the content */
  // FIXME match contentident_id
  entry = rojig_require_next_entry (self, cancellable, error);
  if (!entry)
    return FALSE;

  const char *content_pathname = peel_entry_pathname (entry, error);
  if (!content_pathname)
    return FALSE;
  if (!g_str_has_suffix (content_pathname, "/05content"))
    return glnx_throw (error, "Malformed contentident: %s", content_pathname);

  const struct stat *stbuf = archive_entry_stat (entry);

  /* Copy the data to a temporary file; a better optimization would be to write
   * the data to the first object, then clone it, but that requires some
   * more libostree API.  As far as I can see, one can't reliably seek with
   * libarchive; only some formats support it, and cpio isn't one of them.
   */
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &tmpf, error))
    return FALSE;

  const size_t total = stbuf->st_size;
  const size_t bufsize = MIN (128*1024, total);
  g_autofree guint8* buf = g_malloc (bufsize);
  size_t bytes_read = 0;
  while (bytes_read < total)
    {
      ssize_t r = archive_read_data (self->archive, buf, MIN (bufsize, total - bytes_read));
      if (r < 0)
        return throw_libarchive_error (error, self->archive);
      if (r == 0)
        break;
      if (glnx_loop_write (tmpf.fd, buf, r) < 0)
        return glnx_throw_errno_prefix (error, "write");
      bytes_read += r;
    }
  g_assert_cmpint (bytes_read, ==, total);
  g_clear_pointer (&buf, g_free);

  const guint n = g_variant_n_children (meta);
  for (guint i = 0; i < n; i++)
    {
      const char *checksum;
      guint32 uid,gid,mode;
      g_autoptr(GVariant) xattrs = NULL;
      g_variant_get_child (meta, i, "(&suuu@a(ayay))", &checksum, &uid, &gid, &mode, &xattrs);
      /* See if we already have this object */
      gboolean has_object;
      if (!ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_FILE, checksum,
                                   &has_object, cancellable, error))
        return FALSE;
      if (has_object)
        continue;
      uid = GUINT32_FROM_BE (uid);
      gid = GUINT32_FROM_BE (gid);
      mode = GUINT32_FROM_BE (mode);

      if (lseek (tmpf.fd, 0, SEEK_SET) < 0)
        return glnx_throw_errno_prefix (error, "lseek");
      g_autoptr(GInputStream) istream = g_unix_input_stream_new (tmpf.fd, FALSE);
      /* Like _ostree_stbuf_to_gfileinfo() - TODO make that public with a
       * better content writing API.
       */
      g_autoptr(GFileInfo) finfo = g_file_info_new ();
      g_file_info_set_attribute_uint32 (finfo, "standard::type", G_FILE_TYPE_REGULAR);
      g_file_info_set_attribute_boolean (finfo, "standard::is-symlink", FALSE);
      g_file_info_set_attribute_uint32 (finfo, "unix::uid", uid);
      g_file_info_set_attribute_uint32 (finfo, "unix::gid", gid);
      g_file_info_set_attribute_uint32 (finfo, "unix::mode", mode);
      g_file_info_set_attribute_uint64 (finfo, "standard::size", total);

      g_autoptr(GInputStream) objstream = NULL;
      guint64 objlen;
      if (!ostree_raw_file_to_content_stream (istream, finfo, xattrs, &objstream,
                                              &objlen, cancellable, error))
        return FALSE;

      g_autofree guchar *csum = NULL;
      if (!ostree_repo_write_content (repo, checksum, objstream, objlen, &csum,
                                      cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
state_transition (RpmOstreeRojigAssembler    *self,
                  const char        *pathname,
                  RojigAssemblerState         new_state,
                  GError           **error)
{
  if (self->state > new_state)
    return glnx_throw (error, "Unexpected state for path: %s", pathname);
  self->state = new_state;
  return TRUE;
}

/* Process new objects included in the rojigRPM */
gboolean
rpmostree_rojig_assembler_write_new_objects (RpmOstreeRojigAssembler    *self,
                                   OstreeRepo        *repo,
                                   GCancellable      *cancellable,
                                   GError           **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Writing new objects", error);
  g_assert_cmpint (self->state, ==, STATE_DIRMETA);

  /* TODO sort objects in order for importing, verify we're not
   * importing an unknown object.
   */
  while (TRUE)
    {
      gboolean eof = FALSE;
      struct archive_entry *entry = NULL;
      if (!rojig_next_entry (self, &eof, &entry, cancellable, error))
        return FALSE;
      if (eof)
        break;
      g_assert (entry); /* Pacify static analysis */
      const char *pathname = peel_entry_pathname (entry, error);
      if (!pathname)
        return FALSE;
      if (g_str_has_prefix (pathname, RPMOSTREE_ROJIG_DIRMETA_DIR "/"))
        {
          if (!state_transition (self, pathname, STATE_DIRMETA, error))
            return FALSE;
          g_autofree char *checksum =
            parse_checksum_from_pathname (pathname + strlen (RPMOSTREE_ROJIG_DIRMETA_DIR "/"), error);
          if (!checksum)
            return FALSE;
          g_autoptr(GVariant) dirmeta = rojig_read_variant (OSTREE_DIRMETA_GVARIANT_FORMAT,
                                                            self->archive, entry,
                                                            cancellable, error);
          g_autofree guint8*csum = NULL;
          if (!ostree_repo_write_metadata (repo, OSTREE_OBJECT_TYPE_DIR_META,
                                           checksum, dirmeta, &csum, cancellable, error))
            return FALSE;
        }
      else if (g_str_has_prefix (pathname, RPMOSTREE_ROJIG_DIRTREE_DIR "/"))
        {
          if (!state_transition (self, pathname, STATE_DIRTREE, error))
            return FALSE;
          g_autofree char *checksum =
            parse_checksum_from_pathname (pathname + strlen (RPMOSTREE_ROJIG_DIRTREE_DIR "/"), error);
          if (!checksum)
            return FALSE;
          g_autoptr(GVariant) dirtree = rojig_read_variant (OSTREE_TREE_GVARIANT_FORMAT,
                                                            self->archive, entry,
                                                            cancellable, error);
          g_autofree guint8*csum = NULL;
          if (!ostree_repo_write_metadata (repo, OSTREE_OBJECT_TYPE_DIR_TREE,
                                           checksum, dirtree, &csum, cancellable, error))
            return FALSE;
        }
      else if (g_str_has_prefix (pathname, RPMOSTREE_ROJIG_NEW_CONTENTIDENT_DIR "/"))
        {
          if (!state_transition (self, pathname, STATE_NEW_CONTENTIDENT, error))
            return FALSE;
          if (!process_contentident (self, repo, entry, pathname, cancellable, error))
            return FALSE;
        }
      else if (g_str_has_prefix (pathname, RPMOSTREE_ROJIG_NEW_DIR "/"))
        {
          if (!state_transition (self, pathname, STATE_NEW, error))
            return FALSE;
          g_autofree char *checksum =
            parse_checksum_from_pathname (pathname + strlen (RPMOSTREE_ROJIG_NEW_DIR "/"), error);
          if (!checksum)
            return FALSE;

          const struct stat *stbuf = archive_entry_stat (entry);
          g_assert_cmpint (stbuf->st_size, >=, 0);

          g_autoptr(GInputStream) archive_stream = _rpm_ostree_libarchive_input_stream_new (self->archive);
          g_autofree guint8*csum = NULL;
          if (!ostree_repo_write_content (repo, checksum, archive_stream,
                                          stbuf->st_size, &csum, cancellable, error))
            return FALSE;
        }
      else if (g_str_has_prefix (pathname, RPMOSTREE_ROJIG_XATTRS_DIR "/"))
        {
          self->next_entry = g_steal_pointer (&entry); /* Stash for next call */
          break;
        }
      else
        return glnx_throw (error, "Unexpected entry: %s", pathname);
    }

  return TRUE;
}

GVariant *
rpmostree_rojig_assembler_get_xattr_table (RpmOstreeRojigAssembler    *self)
{
  g_assert (self->xattrs_table);
  return g_variant_ref (self->xattrs_table);
}

/* Loop over each package, returning its xattr set (as indexes into the xattr table) */
gboolean
rpmostree_rojig_assembler_next_xattrs (RpmOstreeRojigAssembler    *self,
                                       GVariant         **out_objid_to_xattrs,
                                       GCancellable      *cancellable,
                                       GError           **error)
{
  /* If we haven't loaded the xattr string table, do so */
  if (self->state < STATE_XATTRS_TABLE)
    {
      struct archive_entry *entry = rojig_require_next_entry (self, cancellable, error);
      if (!entry)
        return FALSE;

      const char *pathname = peel_entry_pathname (entry, error);
      if (!pathname)
        return FALSE;
      if (!g_str_has_prefix (pathname, RPMOSTREE_ROJIG_XATTRS_TABLE))
        return glnx_throw (error, "Unexpected entry: %s", pathname);

      g_autoptr(GVariant) xattrs_table = rojig_read_variant (RPMOSTREE_ROJIG_XATTRS_TABLE_VARIANT_FORMAT,
                                                             self->archive, entry, cancellable, error);
      if (!xattrs_table)
        return FALSE;
      g_assert (!self->xattrs_table);
      self->xattrs_table = g_steal_pointer (&xattrs_table);
      self->state = STATE_XATTRS_TABLE;
    }

  /* Init output variable for EOF state now */
  *out_objid_to_xattrs = NULL;

  /* Look for an xattr entry */
  gboolean eof = FALSE;
  struct archive_entry *entry = NULL;
  if (!rojig_next_entry (self, &eof, &entry, cancellable, error))
    return FALSE;
  if (eof)
    return TRUE; /* 🔚 Early return */
  g_assert (entry); /* Pacify static analysis */

  const char *pathname = peel_entry_pathname (entry, error);
  if (!pathname)
    return FALSE;
  /* At this point there's nothing left besides xattrs, so throw if it doesn't
   * match that filename pattern.
   */
  if (!g_str_has_prefix (pathname, RPMOSTREE_ROJIG_XATTRS_PKG_DIR "/"))
    return glnx_throw (error, "Unexpected entry: %s", pathname);
  // const char *nevra = pathname + strlen (RPMOSTREE_ROJIG_XATTRS_PKG_DIR "/");
  *out_objid_to_xattrs = rojig_read_variant (RPMOSTREE_ROJIG_XATTRS_PKG_VARIANT_FORMAT,
                                             self->archive, entry, cancellable, error);
  return TRUE;
}

/* Client side lookup for xattrs */
gboolean
rpmostree_rojig_assembler_xattr_lookup (GVariant *xattr_table,
                                        const char *path,
                                        GVariant *xattrs,
                                        GVariant **out_xattrs,
                                        GError  **error)
{
  int pos;
  if (!rpmostree_variant_bsearch_str (xattrs, path, &pos))
    {
      const char *bn = glnx_basename (path);
      if (!rpmostree_variant_bsearch_str (xattrs, bn, &pos))
        {
          // TODO add an "objects to skip" map; currently not found means
          // "don't import"
          *out_xattrs = NULL;
          return TRUE;
          /* rojigdata->caught_error = TRUE; */
          /* return glnx_null_throw (&rojigdata->error, "Failed to find rojig xattrs for path '%s'", path); */
        }
    }
  guint xattr_idx;
  g_variant_get_child (xattrs, pos, "(&su)", NULL, &xattr_idx);
  if (xattr_idx >= g_variant_n_children (xattr_table))
    return glnx_throw (error, "Out of range rojig xattr index %u for path '%s'", xattr_idx, path);
  *out_xattrs = g_variant_get_child_value (xattr_table, xattr_idx);
  return TRUE;
}
