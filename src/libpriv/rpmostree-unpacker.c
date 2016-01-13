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
 * Implements unpacking an RPM.  The design here is to reuse
 * libarchive's RPM support for most of it.  We do however need to
 * look at file capabilities, which are part of the header.
 *
 * Hence we end up with two file descriptors open.
 */

#include "config.h"

#include <pwd.h>
#include <grp.h>
#include <sys/capability.h>
#include "rpmostree-unpacker.h"
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmts.h>
#include <archive.h>
#include <archive_entry.h>


#include <string.h>
#include <stdlib.h>

typedef GObjectClass RpmOstreeUnpackerClass;

struct RpmOstreeUnpacker
{
  GObject parent_instance;
  struct archive *archive;
  int fd;
  gboolean owns_fd;
  Header hdr;
  rpmfi fi;
  GHashTable *fscaps;
  RpmOstreeUnpackerFlags flags;
};

G_DEFINE_TYPE(RpmOstreeUnpacker, rpmostree_unpacker, G_TYPE_OBJECT)

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       archive_error_string (a));
}

static void
rpmostree_unpacker_finalize (GObject *object)
{
  RpmOstreeUnpacker *self = (RpmOstreeUnpacker*)object;
  if (self->archive)
    archive_read_free (self->archive); 
  if (self->fi)
    (void) rpmfiFree (self->fi);
  if (self->owns_fd)
    (void) close (self->fd);
  
  G_OBJECT_CLASS (rpmostree_unpacker_parent_class)->finalize (object);
}

static void
rpmostree_unpacker_class_init (RpmOstreeUnpackerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = rpmostree_unpacker_finalize;
}

static void
rpmostree_unpacker_init (RpmOstreeUnpacker *p)
{
}

typedef int(*archive_setup_func)(struct archive *);

/**
 * rpmostree_rpm2cpio:
 * @fd: An open file descriptor for an RPM package
 * @error: GError
 *
 * Parse CPIO content of @fd via libarchive.  Note that the CPIO data
 * does not capture all relevant filesystem content; for example,
 * filesystem capabilities are part of a separate header, etc.
 */
static struct archive *
rpm2cpio (int fd, GError **error)
{
  gboolean success = FALSE;
  struct archive *ret = NULL;
  guint i;

  ret = archive_read_new ();
  g_assert (ret);

  /* We only do the subset necessary for RPM */
  { archive_setup_func archive_setup_funcs[] =
      { archive_read_support_filter_rpm,
        archive_read_support_filter_lzma,
        archive_read_support_filter_gzip,
        archive_read_support_filter_xz,
        archive_read_support_filter_bzip2,
        archive_read_support_format_cpio };

    for (i = 0; i < G_N_ELEMENTS (archive_setup_funcs); i++)
      {
        if (archive_setup_funcs[i](ret) != ARCHIVE_OK)
          {
            propagate_libarchive_error (error, ret);
            goto out;
          }
      }
  }

  if (archive_read_open_fd (ret, fd, 10240) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, ret);
      goto out;
    }

  success = TRUE;
 out:
  if (success)
    return g_steal_pointer (&ret);
  else
    {
      if (ret)
        (void) archive_read_free (ret);
      return NULL;
    }
}

static gboolean
rpm_parse_hdr_fi (int fd, rpmfi *out_fi, Header *out_header,
                  GError **error)
{
  gboolean ret = FALSE;
  g_autofree char *abspath = g_strdup_printf ("/proc/self/fd/%d", fd);
  FD_t rpmfd;
  rpmfi fi = NULL;
  Header hdr = NULL;
  rpmts ts = NULL;
  int r;

  ts = rpmtsCreate ();
  rpmtsSetVSFlags (ts, _RPMVSF_NOSIGNATURES);

  /* librpm needs Fopenfd */
  rpmfd = Fopen (abspath, "r.fdio");
  if (rpmfd == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to open %s", abspath);
      goto out;
    }
  if (Ferror (rpmfd))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Opening %s: %s",
                   abspath,
                   Fstrerror (rpmfd));
      goto out;
    }

  if ((r = rpmReadPackageFile (ts, rpmfd, abspath, &hdr)) != RPMRC_OK)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Verification of %s failed",
                    abspath);
      goto out;
    }
  
  fi = rpmfiNew (ts, hdr, RPMTAG_BASENAMES, (RPMFI_NOHEADER | RPMFI_FLAGS_INSTALL));
  fi = rpmfiInit (fi, 0);
  *out_fi = g_steal_pointer (&fi);
  *out_header = g_steal_pointer (&hdr);
  ret = TRUE;
 out:
  if (fi != NULL)
    rpmfiFree (fi);
  if (hdr != NULL)
    headerFree (hdr);
  return ret;
}

RpmOstreeUnpacker *
rpmostree_unpacker_new_fd (int fd, RpmOstreeUnpackerFlags flags, GError **error)
{
  RpmOstreeUnpacker *ret = NULL;
  Header hdr = NULL;
  rpmfi fi = NULL;
  struct archive *archive;

  archive = rpm2cpio (fd, error);  
  if (archive == NULL)
    goto out;

  rpm_parse_hdr_fi (fd, &fi, &hdr, error);
  if (fi == NULL)
    goto out;

  ret = g_object_new (RPMOSTREE_TYPE_UNPACKER, NULL);
  ret->fd = fd;
  ret->fi = g_steal_pointer (&fi);
  ret->archive = g_steal_pointer (&archive);
  ret->flags = flags;

 out:
  if (archive)
    archive_read_free (archive);
  if (hdr)
    headerFree (hdr);
  if (fi)
    rpmfiFree (fi);
  return ret;
}

RpmOstreeUnpacker *
rpmostree_unpacker_new_at (int dfd, const char *path, RpmOstreeUnpackerFlags flags, GError **error)
{
  RpmOstreeUnpacker *ret = NULL;
  glnx_fd_close int fd = -1;

  fd = openat (dfd, path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      g_prefix_error (error, "Opening %s: ", path);
      goto out;
    }

  ret = rpmostree_unpacker_new_fd (fd, flags, error);
  if (ret == NULL)
    goto out;

  ret->owns_fd = TRUE;
  fd = -1;

 out:
  return ret;
}

static inline const char *
path_relative (const char *src)
{
  if (src[0] == '.' && src[1] == '/')
    src += 2;
  while (src[0] == '/')
    src++;
  return src;
}

gboolean
rpmostree_unpacker_unpack_to_dfd (RpmOstreeUnpacker *self,
                                  int                rootfs_fd,
                                  GCancellable      *cancellable,
                                  GError           **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) rpmfi_overrides = NULL;
  g_autoptr(GHashTable) hardlinks =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  int i = 0;

  /* Right now as I understand it, we need the owner user/group and
   * possibly filesystem capabilities from the header.
   *
   * Otherwise we can just use the CPIO data.
   */
  rpmfi_overrides = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                           (GDestroyNotify) rpmfiFree);
  for (i = 0; rpmfiNext (self->fi) > 0; i++)
    {
      const char *user = rpmfiFUser (self->fi);
      const char *group = rpmfiFGroup (self->fi);
      const char *fcaps = rpmfiFCaps (self->fi);

      if (g_str_equal (user, "root") && g_str_equal (group, "root")
          && !(fcaps && fcaps[0]))
        continue;

      { rpmfi ficopy = rpmfiInit(self->fi, i);
            
        g_hash_table_insert (rpmfi_overrides, g_strdup (path_relative (rpmfiFN (self->fi))), ficopy);
      }
    }
      
  while (TRUE)
    {
      int r;
      const char *fn;
      struct archive_entry *entry;
      glnx_fd_close int destfd = -1;
      mode_t fmode;
      uid_t owner_uid = 0;
      gid_t owner_gid = 0;
      const struct stat *archive_st;
      const char *hardlink;
      rpmfi fi = NULL;

      r = archive_read_next_header (self->archive, &entry);
      if (r == ARCHIVE_EOF)
        {
          break;
        }
      else if (r != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, self->archive);
          goto out;
        }

      fn = path_relative (archive_entry_pathname (entry));

      archive_st = archive_entry_stat (entry);

      hardlink = archive_entry_hardlink (entry);
      if (hardlink)
        {
          g_hash_table_insert (hardlinks, g_strdup (hardlink), g_strdup (fn));
          continue;
        }

      /* Don't try to mkdir parents of "" (originally /) */
      if (fn[0])
        {
          char *fn_copy = strdupa (fn); /* alloca */
          const char *dname = dirname (fn_copy);

          /* Ensure parent directories exist */
          if (!glnx_shutil_mkdir_p_at (rootfs_fd, dname, 0755, cancellable, error))
            goto out;
        }

      fi = g_hash_table_lookup (rpmfi_overrides, fn);
      fmode = archive_st->st_mode;

      if (S_ISDIR (fmode))
        {
          /* Always ensure we can write and execute directories...since
           * this content should ultimately be read-only entirely, we're
           * just breaking things by dropping write permissions during
           * builds.
           */
          fmode |= 0700;
          /* Don't try to mkdir "" (originally /) */
          if (fn[0])
            {
              g_assert (fn[0] != '/');
              if (!glnx_shutil_mkdir_p_at (rootfs_fd, fn, fmode, cancellable, error))
                goto out;
            }
        }
      else if (S_ISLNK (fmode))
        {
          g_assert (fn[0] != '/');
          if (symlinkat (archive_entry_symlink (entry), rootfs_fd, fn) < 0)
            {
              glnx_set_error_from_errno (error);
              g_prefix_error (error, "Creating %s: ", fn);
              goto out;
            }
        }
      else if (S_ISREG (fmode))
        {
          size_t remain = archive_st->st_size;

          g_assert (fn[0] != '/');
          destfd = openat (rootfs_fd, fn, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
          if (destfd < 0)
            {
              glnx_set_error_from_errno (error);
              g_prefix_error (error, "Creating %s: ", fn);
              goto out;
            }

          while (remain)
            {
              const void *buf;
              size_t size;
              gint64 off;

              r = archive_read_data_block (self->archive, &buf, &size, &off);
              if (r == ARCHIVE_EOF)
                break;
              if (r != ARCHIVE_OK)
                {
                  propagate_libarchive_error (error, self->archive);
                  goto out;
                }

              if (glnx_loop_write (destfd, buf, size) < 0)
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
              remain -= size;
            }
        }
      else
        {
          g_set_error (error,
                      G_IO_ERROR,
                       G_IO_ERROR_FAILED,
                       "RPM contains non-regular/non-symlink file %s",
                       fn);
          goto out;
        }

      if (fi && (self->flags & RPMOSTREE_UNPACKER_FLAGS_OWNER) > 0)
        {
          struct passwd *pwent;
          struct group *grent;
          
          pwent = getpwnam (rpmfiFUser (fi));
          if (pwent == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown user '%s'", rpmfiFUser (fi));
              goto out;
            }
          owner_uid = pwent->pw_uid;

          grent = getgrnam (rpmfiFGroup (fi));
          if (grent == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown group '%s'", rpmfiFGroup (fi));
              goto out;
            }
          owner_gid = grent->gr_gid;

          if (fchownat (rootfs_fd, fn, owner_uid, owner_gid, AT_SYMLINK_NOFOLLOW) < 0)
            {
              glnx_set_error_from_errno (error);
              g_prefix_error (error, "fchownat: ");
              goto out;
            }
        }

      if (S_ISREG (fmode))
        {
          if ((self->flags & RPMOSTREE_UNPACKER_FLAGS_SUID_FSCAPS) == 0)
            fmode &= 0777;
          else if (fi != NULL)
            {
              const char *fcaps = rpmfiFCaps (fi);
              if (fcaps != NULL && fcaps[0])
                {
                  cap_t caps = cap_from_text (fcaps);
                  if (cap_set_fd (destfd, caps) != 0)
                    {
                      glnx_set_error_from_errno (error);
                      g_prefix_error (error, "Setting capabilities: ");
                      goto out;
                    }
                }
            }
      
          if (fchmod (destfd, fmode) < 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  { GHashTableIter hashiter;
    gpointer k,v;

    g_hash_table_iter_init (&hashiter, hardlinks);

    while (g_hash_table_iter_next (&hashiter, &k, &v))
      {
        const char *src = path_relative (k);
        const char *dest = path_relative (v);
    
        if (linkat (rootfs_fd, src, rootfs_fd, dest, 0) < 0)
          {
            glnx_set_error_from_errno (error);
            goto out;
          }
      }
  }

  ret = TRUE;
 out:
  return ret;
}
