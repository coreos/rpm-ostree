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
  GString *tmpfiles_d;
  RpmOstreeUnpackerFlags flags;
  DnfPackage *pkg;
  char *hdr_sha256;

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
  if (self->hdr)
    headerFree (self->hdr);
  if (self->archive)
    archive_read_free (self->archive); 
  if (self->fi)
    (void) rpmfiFree (self->fi);
  if (self->owns_fd && self->fd != -1)
    (void) close (self->fd);
  g_string_free (self->tmpfiles_d, TRUE);
  g_free (self->ostree_branch);

  g_hash_table_unref (self->rpmfi_overrides);

  g_free (self->hdr_sha256);

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
  self->tmpfiles_d = g_string_new ("");
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
  _cleanup_rpmts_ rpmts ts = NULL;
  FD_t rpmfd;
  int r;
  _cleanup_rpmheader_ Header ret_header = NULL;
  _cleanup_rpmfi_ rpmfi ret_fi = NULL;
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

      if ((user == NULL || g_str_equal (user, "root")) &&
          (user == NULL || g_str_equal (group, "root")) &&
          (fcaps == NULL || fcaps[0] == '\0'))
        continue;

      g_hash_table_insert (self->rpmfi_overrides, g_strdup (fn),
                           GINT_TO_POINTER (i));
    }
}

/*
 * rpmostree_unpacker_new_fd:
 * @fd: Fd
 * @pkg: (optional): Package reference, used for metadata
 * @flags: flags
 * @error: error
 *
 * Create a new unpacker instance.  The @pkg argument, if
 * specified, will be inspected and metadata such as the
 * origin repo will be added to the final commit.
 */
RpmOstreeUnpacker *
rpmostree_unpacker_new_fd (int fd,
                           DnfPackage *pkg,
                           RpmOstreeUnpackerFlags flags,
                           GError **error)
{
  RpmOstreeUnpacker *ret = NULL;
  _cleanup_rpmheader_ Header hdr = NULL;
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
  ret->pkg = pkg ? g_object_ref (pkg) : NULL;

  build_rpmfi_overrides (ret);

 out:
  if (archive)
    archive_read_free (archive);
  if (fi)
    rpmfiFree (fi);
  return ret;
}

/*
 * rpmostree_unpacker_new_at:
 * @dfd: Fd
 * @path: Path
 * @pkg: (optional): Package reference, used for metadata
 * @flags: flags
 * @error: error
 *
 * Create a new unpacker instance.  The @pkg argument, if
 * specified, will be inspected and metadata such as the
 * origin repo will be added to the final commit.
 */
RpmOstreeUnpacker *
rpmostree_unpacker_new_at (int dfd, const char *path,
                           DnfPackage *pkg,
                           RpmOstreeUnpackerFlags flags,
                           GError **error)
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

  ret = rpmostree_unpacker_new_fd (fd, pkg, flags, error);
  if (ret == NULL)
    goto out;

  ret->owns_fd = TRUE;
  fd = -1;

 out:
  return ret;
}

static void
get_rpmfi_override (RpmOstreeUnpacker *self,
                    const char        *path,
                    const char       **out_user,
                    const char       **out_group,
                    const char       **out_fcaps)
{
  gpointer v;

  /* Note: we use extended here because the value might be index 0 */
  if (!g_hash_table_lookup_extended (self->rpmfi_overrides, path, NULL, &v))
    return;

  rpmfiInit (self->fi, GPOINTER_TO_INT (v));
  g_assert (rpmfiNext (self->fi) >= 0);

  if (out_user)
    *out_user = rpmfiFUser (self->fi);
  if (out_group)
    *out_group = rpmfiFGroup (self->fi);
  if (out_fcaps)
    *out_fcaps = rpmfiFCaps (self->fi);
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

static GVariant *
repo_metadata_to_variant (DnfRepo *repo)
{
  g_auto(GVariantBuilder) builder;
  g_variant_builder_init (&builder, (GVariantType*)"a{sv}");

  /* For now, just the id...in the future maybe we'll add more, but this is
   * enough to provide useful semantics.
   */
  g_variant_builder_add (&builder, "{sv}",
                         "id", g_variant_new_string (dnf_repo_get_id (repo)));

  return g_variant_builder_end (&builder);
}

static gboolean
build_metadata_variant (RpmOstreeUnpacker *self,
                        OstreeSePolicy    *sepolicy,
                        GVariant         **out_variant,
                        GCancellable      *cancellable,
                        GError           **error)
{
  g_autoptr(GChecksum) pkg_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_auto(GVariantBuilder) metadata_builder;
  g_variant_builder_init (&metadata_builder, (GVariantType*)"a{sv}");

  /* NB: We store the full header of the RPM in the commit for three reasons:
   *   1. it holds the file security capabilities, which we need during checkout
   *   2. we'll need to provide it to librpm when it updates the rpmdb (see
   *      rpmostree_context_assemble_commit())
   *   3. it's needed in the local pkgs paths to fool the libdnf stack (see
   *      rpmostree_context_prepare_install())
   */
  {
    g_autoptr(GBytes) metadata = NULL;

    if (!get_lead_sig_header_as_bytes (self, &metadata, cancellable, error))
      return FALSE;

    g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.metadata",
                           g_variant_new_from_bytes ((GVariantType*)"ay",
                                                     metadata, TRUE));

    g_checksum_update (pkg_checksum, g_bytes_get_data (metadata, NULL),
                                     g_bytes_get_size (metadata));

    self->hdr_sha256 = g_strdup (g_checksum_get_string (pkg_checksum));

    g_variant_builder_add (&metadata_builder, "{sv}",
                           "rpmostree.metadata_sha256",
                           g_variant_new_string (self->hdr_sha256));
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

  /* Originally we just had unpack_version = 1, let's add a minor version for
   * compatible increments.
   */
  g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.unpack_minor_version",
                         g_variant_new_uint32 (3));

  if (self->pkg)
    {
      DnfRepo *repo = dnf_package_get_repo (self->pkg);
      if (repo)
        {
          g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.repo",
                                 repo_metadata_to_variant (repo));
        }

      /* include a checksum of the RPM as a whole; the actual algo used depends
       * on how the repodata was created, so just keep a repr */
      g_autofree char* chksum_repr = NULL;
      if (!rpmostree_get_repodata_chksum_repr (self->pkg, &chksum_repr, error))
        return FALSE;

      g_variant_builder_add (&metadata_builder, "{sv}",
                             "rpmostree.repodata_checksum",
                             g_variant_new_string (chksum_repr));
    }

  *out_variant = g_variant_builder_end (&metadata_builder);
  return TRUE;
}

typedef struct
{
  RpmOstreeUnpacker  *self;
  GError  **error;
} cb_data;

/* See this bug:
 * https://bugzilla.redhat.com/show_bug.cgi?id=517575
 */
static void
workaround_fedora_rpm_permissions (GFileInfo *file_info)
{
  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
    {
      guint32 mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
      mode |= S_IWUSR;
      g_file_info_set_attribute_uint32 (file_info, "unix::mode", mode);
    }
}

static void
append_tmpfiles_d (RpmOstreeUnpacker *self,
                   const char *path,
                   GFileInfo *finfo,
                   const char *user,
                   const char *group)
{
  GString *tmpfiles_d = self->tmpfiles_d;
  const guint32 mode = g_file_info_get_attribute_uint32 (finfo, "unix::mode");
  char filetype_c;

  switch (g_file_info_get_file_type (finfo))
    {
    case G_FILE_TYPE_DIRECTORY:
      filetype_c = 'd';
      break;
    case G_FILE_TYPE_SYMBOLIC_LINK:
      filetype_c = 'L';
      break;
    default:
      return;
    }

  g_string_append_c (tmpfiles_d, filetype_c);
  g_string_append_c (tmpfiles_d, ' ');
  g_string_append (tmpfiles_d, path);
  
  switch (g_file_info_get_file_type (finfo))
    {
    case G_FILE_TYPE_DIRECTORY:
      {
        g_string_append_printf (tmpfiles_d, " 0%02o", mode & ~S_IFMT);
        g_string_append_printf (tmpfiles_d, " %s %s - -", user, group);
        g_string_append_c (tmpfiles_d, '\n');
      }
      break;
    case G_FILE_TYPE_SYMBOLIC_LINK:
      {
        const char *target = g_file_info_get_symlink_target (finfo);
        g_string_append (tmpfiles_d, " - - - - ");
        g_string_append (tmpfiles_d, target);
        g_string_append_c (tmpfiles_d, '\n');
      }
      break;
    default:
      g_assert_not_reached ();
    }
}

/* When we do a unified core, we'll likely need to add /boot to pick up
 * kernels here at least.  This is intended short term to address
 * https://github.com/projectatomic/rpm-ostree/issues/233
 */
static gboolean
path_is_ostree_compliant (const char *path)
{
  g_assert (*path == '/');
  path++;
  return (*path == '\0' ||
          g_str_equal (path, "usr")   || g_str_has_prefix (path, "usr/")  ||
          g_str_equal (path, "bin")   || g_str_has_prefix (path, "bin/")  ||
          g_str_equal (path, "sbin")  || g_str_has_prefix (path, "sbin/") ||
          g_str_equal (path, "lib")   || g_str_has_prefix (path, "lib/")  ||
          g_str_equal (path, "lib64") || g_str_has_prefix (path, "lib64/"));
}

static OstreeRepoCommitFilterResult
compose_filter_cb (OstreeRepo         *repo,
                   const char         *path,
                   GFileInfo          *file_info,
                   gpointer            user_data)
{
  RpmOstreeUnpacker *self = ((cb_data*)user_data)->self;
  GError **error = ((cb_data*)user_data)->error;

  const char *user = NULL;
  const char *group = NULL;

  guint32 uid = g_file_info_get_attribute_uint32 (file_info, "unix::uid");
  guint32 gid = g_file_info_get_attribute_uint32 (file_info, "unix::gid");

  gboolean error_was_set = (error && *error != NULL);

  get_rpmfi_override (self, path, &user, &group, NULL);

  /* convert /run and /var entries to tmpfiles.d */
  if (g_str_has_prefix (path, "/run/") ||
      g_str_has_prefix (path, "/var/"))
    {
      append_tmpfiles_d (self, path, file_info,
                         user ?: "root", group ?: "root");
      return OSTREE_REPO_COMMIT_FILTER_SKIP;
    }
  else if (!error_was_set)
    {
      /* sanity check that RPM isn't using CPIO id fields */
      if (uid != 0 || gid != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "RPM had unexpected non-root owned path \"%s\", marked as %u:%u)", path, uid, gid);
          return OSTREE_REPO_COMMIT_FILTER_SKIP;
        }
      /* And ensure the RPM installs into supported paths */
      else if (!path_is_ostree_compliant (path))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unsupported path: %s; See %s",
                       path, "https://github.com/projectatomic/rpm-ostree/issues/233");
          return OSTREE_REPO_COMMIT_FILTER_SKIP;
        }
    }

  workaround_fedora_rpm_permissions (file_info);

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

static OstreeRepoCommitFilterResult
unprivileged_filter_cb (OstreeRepo         *repo,
                        const char         *path,
                        GFileInfo          *file_info,
                        gpointer            user_data)
{
  workaround_fedora_rpm_permissions (file_info);
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

  get_rpmfi_override (self, path, NULL, NULL, &fcaps);

  if (fcaps != NULL && fcaps[0] != '\0')
    return rpmostree_fcap_to_xattr_variant (fcaps);

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
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GFile) root = NULL;
  OstreeRepoCommitModifier *modifier = NULL;
  OstreeRepoImportArchiveOptions opts = { 0 };
  OstreeRepoCommitModifierFlags modifier_flags = 0;
  glnx_unref_object OstreeMutableTree *mtree = NULL;
  char *tmpdir = NULL;
  guint64 buildtime = 0;

  GError *cb_error = NULL;
  cb_data fdata = { self, &cb_error };
  OstreeRepoCommitFilter filter;

  if ((self->flags & RPMOSTREE_UNPACKER_FLAGS_UNPRIVILEGED) > 0)
    filter = unprivileged_filter_cb;
  else
    filter = compose_filter_cb;

  /* If changing this, also look at changing rpmostree-postprocess.c */
  modifier_flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_ERROR_ON_UNLABELED;
  modifier = ostree_repo_commit_modifier_new (modifier_flags, filter, &fdata, NULL);
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

  /* Handle any data we've accumulated data to write to tmpfiles.d.
   * I originally tried to do this entirely in memory but things
   * like selinux labeling only happen as callbacks out of using
   * the input dfd/archive paths...so let's just use a tempdir. (:sadface:)
   */
  if (self->tmpfiles_d->len > 0)
    {
      glnx_fd_close int tmpdir_dfd = -1;
      g_autofree char *pkgname = headerGetAsString (self->hdr, RPMTAG_NAME);

      tmpdir = strdupa ("/tmp/rpm-ostree-import.XXXXXX");
      if (g_mkdtemp_full (tmpdir, 0755) < 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }

      if (!glnx_opendirat (AT_FDCWD, tmpdir, TRUE, &tmpdir_dfd, error))
        goto out;
      if (!glnx_shutil_mkdir_p_at (tmpdir_dfd, "usr/lib/tmpfiles.d", 0755, cancellable,  error))
        goto out;
      if (!glnx_file_replace_contents_at (tmpdir_dfd, glnx_strjoina ("usr/lib/tmpfiles.d/", "pkg-", pkgname, ".conf"),
                                          (guint8*)self->tmpfiles_d->str, self->tmpfiles_d->len, GLNX_FILE_REPLACE_NODATASYNC,
                                          cancellable, error))
        goto out;

      if (!ostree_repo_write_dfd_to_mtree (repo, tmpdir_dfd, ".", mtree, modifier,
                                           cancellable, error))
        goto out;

      /* check if any of the cbs set an error */
      if (cb_error != NULL)
        {
          *error = cb_error;
          goto out;
        }
    }
    
  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    goto out;
  
  if (!build_metadata_variant (self, sepolicy, &metadata, cancellable, error))
    goto out;
  g_variant_ref_sink (metadata);

  /* Use the build timestamp for the commit: this ensures that committing the
   * same RPM always yields the same checksum, which is a useful property to
   * have (barring changes in the unpacker, in which case we wouldn't want the
   * same checksum anyway). */
  buildtime = headerGetNumber (self->hdr, RPMTAG_BUILDTIME);

  if (!ostree_repo_write_commit_with_time (repo, NULL, "", "", metadata,
                                           OSTREE_REPO_FILE (root), buildtime,
                                           out_csum, cancellable, error))
    goto out;

  ret = TRUE;
out:
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
  if (tmpdir)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmpdir, NULL, NULL);
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

  if (out_csum)
    *out_csum = g_steal_pointer (&csum);

  ret = TRUE;
 out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}

char *
rpmostree_unpacker_get_nevra (RpmOstreeUnpacker *self)
{
  if (self->hdr == NULL)
    return NULL;
  return rpmostree_pkg_custom_nevra_strdup (self->hdr,
                                            PKG_NEVRA_FLAGS_NAME |
                                            PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE |
                                            PKG_NEVRA_FLAGS_ARCH);
}

const char *
rpmostree_unpacker_get_header_sha256 (RpmOstreeUnpacker *self)
{
  return self->hdr_sha256;
}
