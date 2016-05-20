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
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmts.h>
#include <archive.h>
#include <archive_entry.h>


#include <string.h>
#include <stdlib.h>
#include <selinux/selinux.h>

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
  GHashTable *rpmfi_overrides;
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

  g_hash_table_unref (self->rpmfi_overrides);

  G_OBJECT_CLASS (rpmostree_unpacker_parent_class)->finalize (object);
}

static void
rpmostree_unpacker_class_init (RpmOstreeUnpackerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = rpmostree_unpacker_finalize;
}

static void
rpmostree_unpacker_init (RpmOstreeUnpacker *self)
{
  self->rpmfi_overrides = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, NULL);
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

static void
build_rpmfi_overrides (RpmOstreeUnpacker *self)
{
  int i;

  /* Right now as I understand it, we need the owner user/group and
   * possibly filesystem capabilities from the header.
   *
   * Otherwise we can just use the CPIO data.
   */
  while ((i = rpmfiNext (self->fi)) >= 0)
    {
      const char *user = rpmfiFUser (self->fi);
      const char *group = rpmfiFGroup (self->fi);
      const char *fcaps = rpmfiFCaps (self->fi);
      const char *fn = rpmfiFN (self->fi);

      if (g_str_equal (user, "root") &&
          g_str_equal (group, "root") &&
          (fcaps == NULL || fcaps[0] == '\0'))
        continue;

      g_hash_table_insert (self->rpmfi_overrides, g_strdup (fn),
                           GINT_TO_POINTER (i));
    }
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

  build_rpmfi_overrides (ret);

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

static gboolean
get_rpmfi_override (RpmOstreeUnpacker *self,
                    const char        *path,
                    const char       **out_user,
                    const char       **out_group,
                    const char       **out_fcaps)
{
  gpointer v;

  /* Note: we use extended here because the value might be index 0 */
  if (!g_hash_table_lookup_extended (self->rpmfi_overrides, path, NULL, &v))
    return FALSE;

  rpmfiInit (self->fi, GPOINTER_TO_INT (v));
  g_assert (rpmfiNext (self->fi) >= 0);

  if (out_user)
    *out_user = rpmfiFUser (self->fi);
  if (out_group)
    *out_group = rpmfiFGroup (self->fi);
  if (out_fcaps)
    *out_fcaps = rpmfiFCaps (self->fi);

  return TRUE;
}

const char *
rpmostree_unpacker_get_ostree_branch (RpmOstreeUnpacker *self)
{
  if (!self->ostree_branch)
    self->ostree_branch = rpmostree_get_cache_branch_header (self->hdr);

  return self->ostree_branch;
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
                   "Failed to read %" G_GSIZE_FORMAT " bytes of metadata",
                   bytes_remaining);
      goto out;
    }
  
  ret = TRUE;
  *out_metadata = g_bytes_new_take (g_steal_pointer (&buf), self->cpio_offset);
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

static gboolean
build_metadata_variant (RpmOstreeUnpacker *self,
                        OstreeSePolicy    *sepolicy,
                        GVariant         **out_variant,
                        GCancellable      *cancellable,
                        GError           **error)
{
  g_auto(GVariantBuilder) metadata_builder;
  g_variant_builder_init (&metadata_builder, (GVariantType*)"a{sv}");

  /* NB: We store the full header of the RPM in the commit for two reasons:
   * first, it holds the file security capabilities, and secondly, we'll need to
   * provide it to librpm when it updates the rpmdb (see
   * rpmostree_context_assemble_commit()). */
  {
    g_autoptr(GBytes) metadata = NULL;

    if (!get_lead_sig_header_as_bytes (self, &metadata, cancellable, error))
      return FALSE;

    g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.metadata",
                           g_variant_new_from_bytes ((GVariantType*)"ay",
                                                     metadata, TRUE));
  }

  /* The current sepolicy that was used to label the unpacked files is important
   * to record. It will help us during future overlays to determine whether the
   * files should be relabeled. */
  if (sepolicy)
    g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.sepolicy",
                           g_variant_new_string
                             (ostree_sepolicy_get_csum (sepolicy)));

  /* let's be nice to our future selves just in case */
  g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.unpack_version",
                         g_variant_new_uint32 (1));

  *out_variant = g_variant_builder_end (&metadata_builder);
  return TRUE;
}

typedef struct
{
  RpmOstreeUnpacker  *self;
  GError  **error;
} cb_data;

static OstreeRepoCommitFilterResult
filter_cb (OstreeRepo         *repo,
           const char         *path,
           GFileInfo          *file_info,
           gpointer            user_data)
{
  RpmOstreeUnpacker *self = ((cb_data*)user_data)->self;
  GError **error = ((cb_data*)user_data)->error;

  /* For now we fail if an RPM requires a file to be owned by non-root. The
   * problem is that RPM provides strings, but ostree records uids/gids. Any
   * mapping we choose would be specific to a certain userdb and thus not
   * portable. To properly support this will probably require switching over to
   * systemd-sysusers: https://github.com/projectatomic/rpm-ostree/issues/49 */

  const char *user = NULL;
  const char *group = NULL;

  guint32 uid = g_file_info_get_attribute_uint32 (file_info, "unix::uid");
  guint32 gid = g_file_info_get_attribute_uint32 (file_info, "unix::gid");

  gboolean was_null = (*error == NULL);

  if (*error == NULL &&
      get_rpmfi_override (self, path, &user, &group, NULL) &&
      (user != NULL || group != NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "path \"%s\" marked as %s%s%s)",
                   path, user, group ? ":" : "", group ?: "");
    }

  /* This should normally never happen since by design RPM doesn't use ids at
   * all, but might as well check since it's right there. */
  if (*error == NULL && (uid != 0 || gid != 0))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "path \"%s\" marked as %u:%u)", path, uid, gid);
    }

  if (was_null && *error != NULL)
    g_prefix_error (error, "Non-root ownership currently unsupported");

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

static GVariant*
xattr_cb (OstreeRepo  *repo,
          const char  *path,
          GFileInfo   *file_info,
          gpointer     user_data)
{
  RpmOstreeUnpacker *self = user_data;
  const char *fcaps = NULL;

  if (get_rpmfi_override (self, path, NULL, NULL, &fcaps) && fcaps != NULL)
    {
      g_auto(GVariantBuilder) builder;
      cap_t caps = cap_from_text (fcaps);
      struct vfs_cap_data vfscap = { 0, };
      g_autoptr(GBytes) vfsbytes = NULL;
      int vfscap_size;

      cap_t_to_vfs (caps, &vfscap, &vfscap_size);
      vfsbytes = g_bytes_new (&vfscap, vfscap_size);

      g_variant_builder_init (&builder, (GVariantType*)"a(ayay)");
      g_variant_builder_add (&builder, "(@ay@ay)",
                             g_variant_new_bytestring ("security.capability"),
                             g_variant_new_from_bytes ((GVariantType*)"ay",
                                                       vfsbytes, FALSE));
      return g_variant_ref_sink (g_variant_builder_end (&builder));
    }

  return NULL;
}

static gboolean
import_rpm_to_repo (RpmOstreeUnpacker *self,
                    OstreeRepo        *repo,
                    OstreeSePolicy    *sepolicy,
                    char             **out_csum,
                    GCancellable      *cancellable,
                    GError           **error)
{
  gboolean ret = FALSE;
  GVariant *metadata = NULL; /* floating */
  g_autoptr(GFile) root = NULL;
  OstreeRepoCommitModifier *modifier = NULL;
  OstreeRepoImportArchiveOptions opts = { 0 };
  glnx_unref_object OstreeMutableTree *mtree = NULL;

  GError *cb_error = NULL;
  cb_data fdata = { self, &cb_error };

  modifier = ostree_repo_commit_modifier_new (0, filter_cb, &fdata, NULL);
  ostree_repo_commit_modifier_set_xattr_callback (modifier, xattr_cb,
                                                  NULL, self);
  ostree_repo_commit_modifier_set_sepolicy (modifier, sepolicy);

  opts.ignore_unsupported_content = TRUE;
  opts.autocreate_parents = TRUE;
  opts.use_ostree_convention =
    (self->flags & RPMOSTREE_UNPACKER_FLAGS_OSTREE_CONVENTION);

  mtree = ostree_mutable_tree_new ();

  if (!ostree_repo_import_archive_to_mtree (repo, &opts, self->archive, mtree,
                                            modifier, cancellable, error))
    goto out;

  /* check if any of the cbs set an error */
  if (cb_error != NULL)
    {
      *error = cb_error;
      goto out;
    }

  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    goto out;

  if (!build_metadata_variant (self, sepolicy, &metadata, cancellable, error))
    goto out;

  if (!ostree_repo_write_commit (repo, NULL, "", "", metadata,
                                 OSTREE_REPO_FILE (root), out_csum,
                                 cancellable, error))
    goto out;

  ret = TRUE;
out:
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
  return ret;
}

gboolean
rpmostree_unpacker_unpack_to_ostree (RpmOstreeUnpacker *self,
                                     OstreeRepo        *repo,
                                     OstreeSePolicy    *sepolicy,
                                     char             **out_csum,
                                     GCancellable      *cancellable,
                                     GError           **error)
{
  gboolean ret = FALSE;
  g_autofree char *csum = NULL;
  const char *branch = NULL;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if (!import_rpm_to_repo (self, repo, sepolicy, &csum, cancellable, error))
    goto out;

  branch = rpmostree_unpacker_get_ostree_branch (self);
  ostree_repo_transaction_set_ref (repo, NULL, branch, csum);

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  *out_csum = g_steal_pointer (&csum);

  ret = TRUE;
 out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}

