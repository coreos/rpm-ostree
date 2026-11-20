/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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

/**
 * Core routines for reading RPM payloads as libarchive streams.
 */

#include "config.h"

#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker-core.h"
#include <rpm/rpmfi.h>
#include <rpm/rpmio.h>

/**
 * throw_libarchive_error:
 * @ar: An archive
 * @error: GError
 * @prefix: Optional error prefix
 *
 * Transform a libarchive error into a GLib error.
 *
 * Return: Always NULL
 */
static struct archive *
throw_libarchive_error (struct archive *ar, GError **error, char const *prefix)
{
  g_assert (ar != NULL);

  const char *err_string = archive_error_string (ar);
  g_assert (err_string != NULL);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, err_string);

  if (prefix != NULL)
    glnx_prefix_error (error, "%s", prefix);

  return NULL;
}

typedef int (*archive_setup_func) (struct archive *);

/**
 * rpmostree_unpack_rpm2cpio:
 * @fd: An open file descriptor for an RPM package
 * @error: GError
 *
 * Parse CPIO content of @fd via libarchive.  Note that the CPIO data
 * does not capture all relevant filesystem content; for example,
 * filesystem capabilities are part of a separate header, etc.
 */
static struct archive *
rpmostree_unpack_rpm2cpio (int fd, GError **error)
{
  g_autoptr (archive) ar = archive_read_new ();
  if (ar == NULL)
    return (struct archive *)glnx_null_throw (error,
                                              "Failed to initialize rpm2cpio archive object");

  /* We only do the subset necessary for RPM */
  {
    archive_setup_func archive_setup_funcs[]
        = { archive_read_support_filter_rpm,   archive_read_support_filter_lzma,
            archive_read_support_filter_gzip,  archive_read_support_filter_xz,
            archive_read_support_filter_bzip2,
#ifdef HAVE_LIBARCHIVE_ZSTD
            archive_read_support_filter_zstd,
#endif
            archive_read_support_format_cpio };

    for (guint i = 0; i < G_N_ELEMENTS (archive_setup_funcs); i++)
      {
        if (archive_setup_funcs[i](ar) != ARCHIVE_OK)
          return throw_libarchive_error (ar, error, "Setting up rpm2cpio");
      }
  }

  if (archive_read_open_fd (ar, fd, 10240) != ARCHIVE_OK)
    return throw_libarchive_error (ar, error, "Reading rpm2cpio");

  return util::move_nullify (ar);
}

/* libostree consumes a libarchive reader. For RPM v6, stream a PAX archive
 * from librpm's payload iterator so we can reuse libostree's import and
 * filtering without materializing the unpacked payload on disk. */
struct RpmArchive
{
  struct archive *writer;
  struct archive_entry *entry;
  FD_t payload;
  rpmfi fi;
  rpmfi metadata;
  RpmOstreeRpmFileFilter filter;
  gpointer filter_data;
  int hardlink_target;
  GCancellable *cancellable;
  GByteArray *output;
  char *hardlink;
  rpm_loff_t remaining;
  bool finished;
};

static void
rpm_archive_free (RpmArchive *state)
{
  if (state->writer != NULL)
    {
      if (!state->finished)
        archive_write_fail (state->writer);
      archive_write_free (state->writer);
    }
  archive_entry_free (state->entry);
  rpmfiFree (state->fi);
  rpmfiFree (state->metadata);
  Fclose (state->payload);
  g_clear_object (&state->cancellable);
  g_byte_array_unref (state->output);
  g_free (state->hardlink);
  g_free (state);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RpmArchive, rpm_archive_free)

static int
rpm_archive_close (struct archive *reader, void *data)
{
  rpm_archive_free (static_cast<RpmArchive *> (data));
  return ARCHIVE_OK;
}

static la_ssize_t
rpm_archive_write (struct archive *writer, void *data, const void *buffer, size_t size)
{
  auto *output = static_cast<GByteArray *> (data);
  g_byte_array_append (output, static_cast<const guint8 *> (buffer), size);
  return size;
}

static int
rpm_archive_write_header (RpmArchive *state)
{
  rpmfi fi = state->fi;
  const int source_index = rpmfiFX (fi);
  int entry_index = source_index;
  const bool has_content = rpmfiArchiveHasContent (fi);

  if (S_ISREG (rpmfiFMode (fi)) && rpmfiFNlink (fi) > 1)
    {
      const int *links = NULL;
      const auto nlinks = rpmfiFLinks (fi, &links);
      if (has_content)
        {
          /* The payload iterator delivers a group's bytes with its first name.
           * Swap that name with a retained member, leaving the payload iterator
           * in place so the content is still read exactly once. */
          state->hardlink_target = source_index;
          for (guint i = 0; i < nlinks; i++)
            {
              rpmfiSetFX (state->metadata, links[i]);
              gboolean skip = FALSE;
              g_autoptr (GError) error = NULL;
              if (!state->filter (state->metadata, &skip, state->filter_data, &error))
                {
                  archive_set_error (state->writer, EIO, "Filtering RPM hardlink: %s",
                                     error->message);
                  return ARCHIVE_FATAL;
                }
              if (!skip)
                {
                  state->hardlink_target = links[i];
                  break;
                }
            }
          entry_index = state->hardlink_target;
          rpmfiSetFX (state->metadata, entry_index);
          g_free (state->hardlink);
          state->hardlink = g_strconcat (".", rpmfiFN (state->metadata), NULL);
        }
      else if (source_index == state->hardlink_target)
        entry_index = links[0];
    }

  /* Metadata follows the emitted name, while file data follows the payload. */
  rpmfi metadata = state->metadata;
  rpmfiSetFX (metadata, entry_index);
  const char *path = rpmfiFN (metadata);
  if (path == NULL || path[0] != '/')
    {
      archive_set_error (state->writer, EINVAL, "Invalid RPM payload path: %s", path ?: "");
      return ARCHIVE_FATAL;
    }

  struct archive_entry *entry = archive_entry_clear (state->entry);
  g_autofree char *relative_path = g_strconcat (".", path, NULL);
  archive_entry_set_pathname (entry, relative_path);
  /* RPM stores ownership by name. The importer handles those names from the
   * header, so keep numeric ownership at zero without consulting the host. */
  const auto mode = rpmfiFMode (metadata);
  archive_entry_set_mode (entry, mode);
  archive_entry_set_mtime (entry, rpmfiFMtime (metadata), 0);
  archive_entry_set_size (entry, 0);
  if (S_ISLNK (mode))
    archive_entry_set_symlink (entry, rpmfiFLink (metadata));

  if (S_ISREG (mode))
    {
      if (has_content)
        {
          state->remaining = rpmfiFSize (fi);
          archive_entry_set_size (entry, state->remaining);
        }
      else
        archive_entry_set_hardlink (entry, state->hardlink);
    }

  return archive_write_header (state->writer, entry);
}

static la_ssize_t
rpm_archive_read (struct archive *reader, void *data, const void **buffer)
{
  auto *state = static_cast<RpmArchive *> (data);
  const auto writer_error = [reader, state] ()
    {
      archive_copy_error (reader, state->writer);
      return -1;
    };
  g_byte_array_set_size (state->output, 0);

  while (state->output->len == 0 && !state->finished)
    {
      if (g_cancellable_is_cancelled (state->cancellable))
        {
          archive_set_error (reader, ECANCELED, "RPM payload import cancelled");
          return -1;
        }

      if (state->remaining > 0)
        {
          char content[64 * 1024];
          auto size = static_cast<size_t> (MIN (state->remaining, sizeof (content)));
          if (rpmfiArchiveRead (state->fi, content, size) != static_cast<ssize_t> (size))
            {
              archive_set_error (reader, EIO, "Reading RPM payload file %s", rpmfiFN (state->fi));
              return -1;
            }
          if (archive_write_data (state->writer, content, size) != static_cast<la_ssize_t> (size))
            return writer_error ();
          state->remaining -= size;
          continue;
        }

      const int index = rpmfiNext (state->fi);
      if (index == RPMERR_ITER_END)
        {
          if (archive_write_close (state->writer) != ARCHIVE_OK)
            return writer_error ();
          state->finished = true;
        }
      else if (index < 0)
        {
          g_autofree char *message = rpmfileStrerror (index);
          archive_set_error (reader, EIO, "Reading RPM payload: %s", message);
          return -1;
        }
      else
        {
          const auto mode = rpmfiFMode (state->fi);
          if ((S_ISREG (mode) || S_ISDIR (mode) || S_ISLNK (mode))
              && rpm_archive_write_header (state) != ARCHIVE_OK)
            return writer_error ();
        }
    }

  *buffer = state->output->data;
  return state->output->len;
}

struct archive *
rpmostree_unpack_rpm (int fd, Header header, off_t payload_offset, RpmOstreeRpmFileFilter filter,
                      gpointer filter_data, GCancellable *cancellable, GError **error)
{
  // RPMTAG_RPMFORMAT is 5114. Use its number because older librpm versions
  // can read v6 payloads without knowing the tag name. V4 headers omit it.
  const rpmTagVal format_tag = 5114;
  if (headerGetNumber (header, format_tag) < 6)
    return rpmostree_unpack_rpm2cpio (fd, error);

  g_autofree char *path = g_strdup_printf ("/proc/self/fd/%d", fd);
  g_auto (FD_t) payload = Fopen (path, "r.ufdio");
  if (payload == NULL || Ferror (payload))
    return (struct archive *)glnx_null_throw (error, "Opening RPM payload: %s",
                                              Fstrerror (payload));
  if (Fseek (payload, payload_offset, SEEK_SET) < 0)
    return (struct archive *)glnx_null_throw (error, "Seeking to RPM payload: %s",
                                              Fstrerror (payload));

  const char *compressor = headerGetString (header, RPMTAG_PAYLOADCOMPRESSOR);
  if (compressor != NULL)
    {
      g_autofree char *mode = g_strdup_printf ("r.%s", compressor);
      if (Fdopen (payload, mode) == NULL || Ferror (payload))
        return (struct archive *)glnx_null_throw (error, "Opening RPM payload with %s: %s",
                                                  compressor, Fstrerror (payload));
    }

  const rpmfiFlags flags = RPMFI_FLAGS_QUERY | RPMFI_NOFILEDIGESTS | RPMFI_NOFILESIGNATURES;
  g_auto (rpmfiles) files = rpmfilesNew (NULL, header, RPMTAG_BASENAMES, flags);
  if (files == NULL)
    return (struct archive *)glnx_null_throw (error, "Reading RPM payload file metadata");

  g_auto (rpmfi) reader
      = rpmfiNewArchiveReader (payload, files, RPMFI_ITER_READ_ARCHIVE_CONTENT_FIRST);
  if (reader == NULL)
    return (struct archive *)glnx_null_throw (error, "Creating RPM payload iterator");

  g_autoptr (archive) ar = archive_read_new ();
  if (ar == NULL)
    return (struct archive *)glnx_null_throw (error, "Creating RPM payload reader");
  if (archive_read_support_format_tar (ar) != ARCHIVE_OK)
    return throw_libarchive_error (ar, error, "Setting up RPM payload reader");

  g_autoptr (RpmArchive) state = g_new0 (RpmArchive, 1);
  state->writer = archive_write_new ();
  state->entry = archive_entry_new ();
  state->payload = util::move_nullify (payload);
  state->fi = util::move_nullify (reader);
  state->metadata = rpmfilesIter (files, RPMFI_ITER_FWD);
  state->filter = filter;
  state->filter_data = filter_data;
  state->cancellable = cancellable ? (GCancellable *)g_object_ref (cancellable) : NULL;
  state->output = g_byte_array_new ();
  if (state->writer == NULL || state->entry == NULL || state->metadata == NULL)
    return (struct archive *)glnx_null_throw (error, "Creating RPM payload writer");
  if (archive_write_set_format_pax_restricted (state->writer) != ARCHIVE_OK
      || archive_write_set_format_option (state->writer, "pax", "hdrcharset", "BINARY")
             != ARCHIVE_OK
      || archive_write_set_bytes_per_block (state->writer, 0) != ARCHIVE_OK
      || archive_write_open (state->writer, state->output, NULL, rpm_archive_write, NULL)
             != ARCHIVE_OK)
    return throw_libarchive_error (state->writer, error, "Setting up RPM payload writer");

  if (archive_read_open (ar, g_steal_pointer (&state), NULL, rpm_archive_read, rpm_archive_close)
      != ARCHIVE_OK)
    return throw_libarchive_error (ar, error, "Opening RPM payload stream");

  return util::move_nullify (ar);
}
