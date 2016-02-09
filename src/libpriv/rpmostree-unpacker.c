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
#include <gio/gunixinputstream.h>
#include "rpmostree-unpacker.h"
#include "rpmostree-core.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-ostree-libarchive-copynpaste.h"
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
  off_t cpio_offset;
  GHashTable *fscaps;
  RpmOstreeUnpackerFlags flags;

  char *ostree_branch;
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
  if (self->owns_fd && self->fd != -1)
    (void) close (self->fd);
  g_free (self->ostree_branch);
  
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

gboolean
rpmostree_unpacker_read_metainfo (int fd,
                                  Header *out_header,
                                  gsize *out_cpio_offset,
                                  rpmfi *out_fi,
                                  GError **error)
{
  gboolean ret = FALSE;
  rpmts ts = NULL;
  FD_t rpmfd;
  int r;
  Header ret_header = NULL;
  rpmfi ret_fi = NULL;
  gsize ret_cpio_offset;
  g_autofree char *abspath = g_strdup_printf ("/proc/self/fd/%d", fd);

  ts = rpmtsCreate ();
  _rpmostree_reset_rpm_sighandlers ();
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

  if ((r = rpmReadPackageFile (ts, rpmfd, abspath, &ret_header)) != RPMRC_OK)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Verification of %s failed",
                    abspath);
      goto out;
    }

  ret_cpio_offset = Ftell (rpmfd);
  
  if (out_fi)
    {
      ret_fi = rpmfiNew (ts, ret_header, RPMTAG_BASENAMES, (RPMFI_NOHEADER | RPMFI_FLAGS_INSTALL));
      ret_fi = rpmfiInit (ret_fi, 0);
    }

  ret = TRUE;
  if (out_header)
    *out_header = g_steal_pointer (&ret_header);
  if (out_fi)
    *out_fi = g_steal_pointer (&ret_fi);
  if (out_cpio_offset)
    *out_cpio_offset = ret_cpio_offset;
 out:
  if (ret_header)
    headerFree (ret_header);
  if (rpmfd)
    (void) Fclose (rpmfd);
  return ret;
}

RpmOstreeUnpacker *
rpmostree_unpacker_new_fd (int fd, RpmOstreeUnpackerFlags flags, GError **error)
{
  RpmOstreeUnpacker *ret = NULL;
  Header hdr = NULL;
  rpmfi fi = NULL;
  struct archive *archive;
  gsize cpio_offset;

  archive = rpm2cpio (fd, error);  
  if (archive == NULL)
    goto out;

  if (!rpmostree_unpacker_read_metainfo (fd, &hdr, &cpio_offset, &fi, error))
    goto out;

  ret = g_object_new (RPMOSTREE_TYPE_UNPACKER, NULL);
  ret->fd = fd;
  ret->fi = g_steal_pointer (&fi);
  ret->archive = g_steal_pointer (&archive);
  ret->flags = flags;
  ret->hdr = g_steal_pointer (&hdr);
  ret->cpio_offset = cpio_offset;

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

static GHashTable *
build_rpmfi_overrides (RpmOstreeUnpacker *self)
{
  g_autoptr(GHashTable) rpmfi_overrides = NULL;
  int i;

  /* Right now as I understand it, we need the owner user/group and
   * possibly filesystem capabilities from the header.
   *
   * Otherwise we can just use the CPIO data.
   */
  rpmfi_overrides = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (i = 0; rpmfiNext (self->fi) >= 0; i++)
    {
      const char *user = rpmfiFUser (self->fi);
      const char *group = rpmfiFGroup (self->fi);
      const char *fcaps = rpmfiFCaps (self->fi);

      if (g_str_equal (user, "root") && g_str_equal (group, "root")
          && !(fcaps && fcaps[0]))
        continue;

      g_hash_table_insert (rpmfi_overrides,
                           g_strdup (path_relative (rpmfiFN (self->fi))),
                           GINT_TO_POINTER (i));
    }
      
  return g_steal_pointer (&rpmfi_overrides);
}

static gboolean
next_archive_entry (struct archive *archive,
                    struct archive_entry **out_entry,
                    GError **error)
{
  int r;

  r = archive_read_next_header (archive, out_entry);
  if (r == ARCHIVE_EOF)
    {
      *out_entry = NULL;
      return TRUE;
    }
  else if (r != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, archive);
      return FALSE;
    }

  return TRUE;
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

  rpmfi_overrides = build_rpmfi_overrides (self);

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

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;

      if (!next_archive_entry (self->archive, &entry, error))
        goto out;
      if (entry == NULL)
        break;

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

      { gpointer v;
        if (g_hash_table_lookup_extended (rpmfi_overrides, fn, NULL, &v))
          {
            int override_fi_idx = GPOINTER_TO_INT (v);
            fi = self->fi;
            rpmfiInit (self->fi, override_fi_idx);
          }
      }
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

      if (fi != NULL && (self->flags & RPMOSTREE_UNPACKER_FLAGS_OWNER) > 0)
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

const char *
rpmostree_unpacker_get_ostree_branch (RpmOstreeUnpacker *self)
{
  if (!self->ostree_branch)
    self->ostree_branch = rpmostree_get_cache_branch_header (self->hdr);

  return self->ostree_branch;
}

static gboolean
write_directory_meta (OstreeRepo   *repo,
                      GFileInfo    *file_info,
                      GVariant     *xattrs,
                      char        **out_checksum,
                      GCancellable *cancellable,
                      GError      **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) dirmeta = NULL;
  g_autofree guchar *csum = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  dirmeta = ostree_create_directory_metadata (file_info, xattrs);

  if (!ostree_repo_write_metadata (repo, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                   dirmeta, &csum, cancellable, error))
    goto out;

  ret = TRUE;
  *out_checksum = ostree_checksum_from_bytes (csum);
 out:
  return ret;
}

struct _cap_struct {
    struct __user_cap_header_struct head;
    union {
	struct __user_cap_data_struct set;
	__u32 flat[3];
    } u[_LINUX_CAPABILITY_U32S_2];
};
/* Rewritten version of _fcaps_save from libcap, since it's not
 * exposed, and we need to generate the raw value.
 */
static void
cap_t_to_vfs (cap_t cap_d, struct vfs_cap_data *rawvfscap, int *out_size)
{
  guint32 eff_not_zero, magic;
  guint tocopy, i;
  
  /* Hardcoded to 2.  There is apparently a version 3 but it just maps
   * to 2.  I doubt another version would ever be implemented, and
   * even if it was we'd need to be backcompatible forever.  Anyways,
   * setuid/fcaps binaries should go away entirely.
   */
  magic = VFS_CAP_REVISION_2;
  tocopy = VFS_CAP_U32_2;
  *out_size = XATTR_CAPS_SZ_2;

  for (eff_not_zero = 0, i = 0; i < tocopy; i++)
    eff_not_zero |= cap_d->u[i].flat[CAP_EFFECTIVE];

  /* Here we're also not validating that the kernel understands
   * the capabilities.
   */

  for (i = 0; i < tocopy; i++)
    {
      rawvfscap->data[i].permitted
        = GUINT32_TO_LE(cap_d->u[i].flat[CAP_PERMITTED]);
      rawvfscap->data[i].inheritable
        = GUINT32_TO_LE(cap_d->u[i].flat[CAP_INHERITABLE]);
    }

  if (eff_not_zero == 0)
    rawvfscap->magic_etc = GUINT32_TO_LE(magic);
  else
    rawvfscap->magic_etc = GUINT32_TO_LE(magic|VFS_CAP_FLAGS_EFFECTIVE);
}

static char *
tweak_path_for_ostree (const char *path)
{
  path = path_relative (path);
  if (g_str_has_prefix (path, "etc/"))
    return g_strconcat ("usr/", path, NULL);
  else if (strcmp (path, "etc") == 0)
    return g_strdup ("usr");
  return g_strdup (path);
}

static gboolean
import_one_libarchive_entry_to_ostree (RpmOstreeUnpacker *self,
                                       GHashTable        *rpmfi_overrides,
                                       OstreeRepo        *repo,
                                       OstreeSePolicy    *sepolicy,
                                       struct archive_entry *entry,
                                       OstreeMutableTree *root,
                                       const char        *default_dir_checksum,
                                       GCancellable      *cancellable,
                                       GError           **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) pathname_parts = NULL;
  g_autofree char *pathname = NULL;
  glnx_unref_object OstreeMutableTree *parent = NULL;
  const char *basename;
  const struct stat *st;
  g_auto(GVariantBuilder) xattr_builder;
  rpmfi fi = NULL;

  g_variant_builder_init (&xattr_builder, (GVariantType*)"a(ayay)");
  
  pathname = tweak_path_for_ostree (archive_entry_pathname (entry));
  st = archive_entry_stat (entry);
  { gpointer v;
    if (g_hash_table_lookup_extended (rpmfi_overrides, pathname, NULL, &v))
      {
        int override_fi_idx = GPOINTER_TO_INT (v);
        fi = self->fi;
        rpmfiInit (self->fi, override_fi_idx);
      }
  }

  if (fi != NULL)
    {
      const char *fcaps = rpmfiFCaps (fi);
      if (fcaps != NULL && fcaps[0])
        {
          cap_t caps = cap_from_text (fcaps);
          struct vfs_cap_data vfscap = { 0, };
          int vfscap_size;
          g_autoptr(GBytes) vfsbytes = NULL;

          cap_t_to_vfs (caps, &vfscap, &vfscap_size);
          vfsbytes = g_bytes_new (&vfscap, vfscap_size);
      
          g_variant_builder_add (&xattr_builder, "(@ay@ay)",
                                 g_variant_new_bytestring ("security.capability"),
                                 g_variant_new_from_bytes ((GVariantType*)"ay",
                                                           vfsbytes,
                                                           FALSE)); 
        }
    }

  if (!pathname[0])
    {
      parent = NULL;
      basename = NULL;
    }
  else
    {
      if (!rpmostree_split_path_ptrarray_validate (pathname, &pathname_parts, error))
        goto out;

      if (default_dir_checksum)
        {
          if (!ostree_mutable_tree_ensure_parent_dirs (root, pathname_parts,
                                                       default_dir_checksum,
                                                       &parent,
                                                       error))
            goto out;
        }
      else
        {
          if (!ostree_mutable_tree_walk (root, pathname_parts, 0, &parent, error))
            goto out;
        }
      basename = (const char*)pathname_parts->pdata[pathname_parts->len-1];
    }

  if (archive_entry_hardlink (entry))
    {
      g_autofree char *hardlink = tweak_path_for_ostree (archive_entry_hardlink (entry));
      const char *hardlink_basename;
      g_autoptr(GPtrArray) hardlink_split_path = NULL;
      glnx_unref_object OstreeMutableTree *hardlink_source_parent = NULL;
      glnx_unref_object OstreeMutableTree *hardlink_source_subdir = NULL;
      g_autofree char *hardlink_source_checksum = NULL;

      g_assert (parent != NULL);

      if (!rpmostree_split_path_ptrarray_validate (hardlink, &hardlink_split_path, error))
        goto out;
      if (hardlink_split_path->len == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid hardlink path %s", hardlink);
          goto out;
        }
      
      hardlink_basename = hardlink_split_path->pdata[hardlink_split_path->len - 1];
      
      if (!ostree_mutable_tree_walk (root, hardlink_split_path, 0, &hardlink_source_parent, error))
        goto out;
      
      if (!ostree_mutable_tree_lookup (hardlink_source_parent, hardlink_basename,
                                       &hardlink_source_checksum,
                                       &hardlink_source_subdir,
                                       error))
        {
          g_prefix_error (error, "While resolving hardlink target: ");
          goto out;
        }
      
      if (hardlink_source_subdir)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Hardlink %s refers to directory %s",
                       pathname, hardlink);
          goto out;
        }
      g_assert (hardlink_source_checksum);
      
      if (!ostree_mutable_tree_replace_file (parent,
                                             basename,
                                             hardlink_source_checksum,
                                             error))
        goto out;
    }
  else
    {
      g_autofree char *object_checksum = NULL;
      g_autoptr(GFileInfo) file_info = NULL;
      glnx_unref_object OstreeMutableTree *subdir = NULL;

      file_info = _rpmostree_libarchive_to_file_info (entry);

      if (S_ISDIR (st->st_mode))
        {
          if (!write_directory_meta (repo, file_info, NULL, &object_checksum,
                                     cancellable, error))
            goto out;

          if (parent == NULL)
            {
              subdir = g_object_ref (root);
            }
          else
            {
              if (!ostree_mutable_tree_ensure_dir (parent, basename, &subdir, error))
                goto out;
            }

          ostree_mutable_tree_set_metadata_checksum (subdir, object_checksum);
        }
      else if (S_ISREG (st->st_mode) || S_ISLNK (st->st_mode))
        {
          g_autofree guchar *object_csum = NULL;

          if (parent == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Can't import file as root directory");
              goto out;
            }

          if (!_rpmostree_import_libarchive_entry_file (repo, self->archive, entry, file_info,
                                                        g_variant_builder_end (&xattr_builder),
                                                        &object_csum,
                                                        cancellable, error))
            goto out;
          
          object_checksum = ostree_checksum_from_bytes (object_csum);
          if (!ostree_mutable_tree_replace_file (parent, basename,
                                                 object_checksum,
                                                 error))
            goto out;
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unsupported file type for path '%s'", pathname);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
get_lead_sig_header_as_bytes (RpmOstreeUnpacker *self,
                              GBytes  **out_metadata,
                              GCancellable *cancellable,
                              GError  **error)
{
  gboolean ret = FALSE;
  glnx_unref_object GInputStream *uin = NULL;
  g_autofree char *buf = NULL;
  char *bufp;
  size_t bytes_remaining;

  /* Inline a pread() based reader here to avoid affecting the file
   * offset since both librpm and libarchive have references.
   */
  buf = g_malloc (self->cpio_offset);
  bufp = buf;
  bytes_remaining = self->cpio_offset;
  while (bytes_remaining > 0)
    {
      ssize_t bytes_read;
      do
        bytes_read = pread (self->fd, bufp, bytes_remaining, bufp - buf);
      while (bytes_read < 0 && errno == EINTR);
      if (bytes_read < 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
      if (bytes_read == 0)
        break;
      bufp += bytes_read;
      bytes_remaining -= bytes_read;
    }
  if (bytes_remaining > 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to read %" G_GUINT64_FORMAT " bytes of metadata",
                   bytes_remaining);
      goto out;
    }
  
  ret = TRUE;
  *out_metadata = g_bytes_new_take (g_steal_pointer (&buf), self->cpio_offset);
 out:
  return ret;
}

gboolean
rpmostree_unpacker_unpack_to_ostree (RpmOstreeUnpacker *self,
                                     OstreeRepo        *repo,
                                     OstreeSePolicy    *sepolicy,
                                     char             **out_commit,
                                     GCancellable      *cancellable,
                                     GError           **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) rpmfi_overrides = NULL;
  g_autoptr(GHashTable) hardlinks =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autofree char *default_dir_checksum  = NULL;
  g_autoptr(GFile) root = NULL;
  glnx_unref_object OstreeMutableTree *mtree = NULL;
  g_autoptr(GBytes) header_bytes = NULL;
  g_autofree char *commit_checksum = NULL;
  g_auto(GVariantBuilder) metadata_builder;

  g_variant_builder_init (&metadata_builder, (GVariantType*)"a{sv}");

  rpmfi_overrides = build_rpmfi_overrides (self);

  g_assert (sepolicy == NULL);

  /* Default directories are 0/0/0755, and right now we're ignoring
   * SELinux.  (This might be a problem for /etc, but in practice
   * anything with nontrivial perms should be in the packages)
   */
  { glnx_unref_object GFileInfo *default_dir_perms  = g_file_info_new ();
    g_file_info_set_attribute_uint32 (default_dir_perms, "unix::uid", 0);
    g_file_info_set_attribute_uint32 (default_dir_perms, "unix::gid", 0);
    g_file_info_set_attribute_uint32 (default_dir_perms, "unix::mode", 0755 | S_IFDIR);
    
    if (!write_directory_meta (repo, default_dir_perms, NULL,
                               &default_dir_checksum, cancellable, error))
      goto out;
  }

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  mtree = ostree_mutable_tree_new ();
  ostree_mutable_tree_set_metadata_checksum (mtree, default_dir_checksum);

  { g_autoptr(GBytes) metadata = NULL;
    
    if (!get_lead_sig_header_as_bytes (self, &metadata, cancellable, error))
      goto out;

    g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.metadata",
                           g_variant_new_from_bytes ((GVariantType*)"ay", metadata, TRUE));
  }
                                                                          
  while (TRUE)
    {
      struct archive_entry *entry;
      
      if (!next_archive_entry (self->archive, &entry, error))
        goto out;
      if (entry == NULL)
        break;

      if (!import_one_libarchive_entry_to_ostree (self, rpmfi_overrides, repo,
                                                  sepolicy, entry, mtree,
                                                  default_dir_checksum,
                                                  cancellable, error))
        goto out;
    }

  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    goto out;

  if (!ostree_repo_write_commit (repo, NULL, "", "",
                                 g_variant_builder_end (&metadata_builder),
                                 OSTREE_REPO_FILE (root),
                                 &commit_checksum, cancellable, error))
    goto out;

  ostree_repo_transaction_set_ref (repo, NULL,
                                   rpmostree_unpacker_get_ostree_branch (self),
                                   commit_checksum);

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
  *out_commit = g_steal_pointer (&commit_checksum);
 out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}

