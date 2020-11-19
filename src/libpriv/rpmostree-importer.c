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
#include "rpmostree-unpacker-core.h"
#include "rpmostree-importer.h"
#include "rpmostree-core.h"
#include "rpmostree-rojig-assembler.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmts.h>
#include <archive.h>
#include <archive_entry.h>


#include <string.h>
#include <stdlib.h>

typedef GObjectClass RpmOstreeImporterClass;

struct RpmOstreeImporter
{
  GObject parent_instance;
  OstreeRepo *repo;
  OstreeSePolicy *sepolicy;
  struct archive *archive;
  int fd;
  Header hdr;
  rpmfi fi;
  off_t cpio_offset;
  GHashTable *rpmfi_overrides;
  GHashTable *doc_files;
  GHashTable *opt_files;
  GString *tmpfiles_d;
  RpmOstreeImporterFlags flags;
  gboolean unpacking_as_nonroot;
  DnfPackage *pkg;
  char *hdr_sha256;

  guint n_rojig_skipped;
  guint n_rojig_total;
  char *ostree_branch;

  gboolean rojig_mode;
  char *rojig_cacheid;
  GVariant *rojig_xattr_table;
  GVariant *rojig_xattrs;
  GVariant *rojig_next_xattrs; /* passed from filter to xattr cb */
};

G_DEFINE_TYPE(RpmOstreeImporter, rpmostree_importer, G_TYPE_OBJECT)

static void
rpmostree_importer_finalize (GObject *object)
{
  RpmOstreeImporter *self = (RpmOstreeImporter*)object;
  if (self->hdr)
    headerFree (self->hdr);
  if (self->archive)
    archive_read_free (self->archive);
  if (self->fi)
    (void) rpmfiFree (self->fi);
  glnx_close_fd (&self->fd);
  g_string_free (self->tmpfiles_d, TRUE);
  g_free (self->ostree_branch);
  g_clear_object (&self->repo);
  g_clear_object (&self->sepolicy);

  g_clear_pointer (&self->rpmfi_overrides, (GDestroyNotify)g_hash_table_unref);
  g_clear_pointer (&self->doc_files, (GDestroyNotify)g_hash_table_unref);
  g_clear_pointer (&self->opt_files, (GDestroyNotify)g_hash_table_unref);

  g_free (self->hdr_sha256);

  g_free (self->rojig_cacheid);
  g_clear_pointer (&self->rojig_xattr_table, (GDestroyNotify)g_variant_unref);
  g_clear_pointer (&self->rojig_xattrs, (GDestroyNotify)g_variant_unref);
  g_clear_pointer (&self->rojig_next_xattrs, (GDestroyNotify)g_variant_unref);

  G_OBJECT_CLASS (rpmostree_importer_parent_class)->finalize (object);
}

static void
rpmostree_importer_class_init (RpmOstreeImporterClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = rpmostree_importer_finalize;
}

static void
rpmostree_importer_init (RpmOstreeImporter *self)
{
  self->fd = -1;
  self->rpmfi_overrides = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, NULL);
  self->tmpfiles_d = g_string_new ("");
}

gboolean
rpmostree_importer_read_metainfo (int fd,
                                  Header *out_header,
                                  gsize *out_cpio_offset,
                                  rpmfi *out_fi,
                                  GError **error)
{
  gboolean ret = FALSE;
  g_auto(rpmts) ts = NULL;
  FD_t rpmfd;
  int r;
  g_auto(Header) ret_header = NULL;
  g_auto(rpmfi) ret_fi = NULL;
  gsize ret_cpio_offset;
  g_autofree char *abspath = g_strdup_printf ("/proc/self/fd/%d", fd);

  DECLARE_RPMSIGHANDLER_RESET;
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
build_rpmfi_overrides (RpmOstreeImporter *self)
{
  int i;

  /* Right now as I understand it, we need the owner user/group and
   * possibly filesystem capabilities from the header.
   *
   * Otherwise we can just use the CPIO data.  Though for handling
   * NODOCS, we gather a hashset of the files with doc flags.
   */
  const gboolean docs_are_filtered = self->doc_files != NULL;
  while ((i = rpmfiNext (self->fi)) >= 0)
    {
      const char *user = rpmfiFUser (self->fi);
      const char *group = rpmfiFGroup (self->fi);
      const char *fcaps = rpmfiFCaps (self->fi);
      const char *fn = rpmfiFN (self->fi);
      rpmfileAttrs fattrs = rpmfiFFlags (self->fi);

      const gboolean user_is_root = (user == NULL || g_str_equal (user, "root"));
      const gboolean group_is_root = (group == NULL || g_str_equal (group, "root"));
      const gboolean fcaps_is_unset = (fcaps == NULL || fcaps[0] == '\0');
      if (!(user_is_root && group_is_root && fcaps_is_unset))
        {
          g_hash_table_insert (self->rpmfi_overrides, g_strdup (fn),
                               GINT_TO_POINTER (i));
        }

      const gboolean is_doc = (fattrs & RPMFILE_DOC) > 0;
      if (docs_are_filtered && is_doc)
        g_hash_table_add (self->doc_files, g_strdup (fn));
    }
}

/*
 * rpmostree_importer_new_take_fd:
 * @fd: Fd
 * @repo: repo
 * @pkg: (optional): Package reference, used for metadata
 * @flags: flags
 * @sepolicy: (optional): SELinux policy
 * @error: error
 *
 * Create a new unpacker instance.  The @pkg argument, if
 * specified, will be inspected and metadata such as the
 * origin repo will be added to the final commit.
 */
RpmOstreeImporter *
rpmostree_importer_new_take_fd (int                     *fd,
                                OstreeRepo              *repo,
                                DnfPackage              *pkg,
                                RpmOstreeImporterFlags   flags,
                                OstreeSePolicy          *sepolicy,
                                GError                 **error)
{
  RpmOstreeImporter *ret = NULL;
  g_auto(Header) hdr = NULL;
  rpmfi fi = NULL;
  struct archive *archive;
  gsize cpio_offset;

  archive = rpmostree_unpack_rpm2cpio (*fd, error);
  if (archive == NULL)
    goto out;

  if (!rpmostree_importer_read_metainfo (*fd, &hdr, &cpio_offset, &fi, error))
    goto out;

  ret = g_object_new (RPMOSTREE_TYPE_IMPORTER, NULL);
  ret->fd = glnx_steal_fd (fd);
  ret->repo = g_object_ref (repo);
  ret->sepolicy = sepolicy ? g_object_ref (sepolicy) : NULL;
  ret->fi = g_steal_pointer (&fi);
  ret->archive = g_steal_pointer (&archive);
  ret->flags = flags;
  ret->unpacking_as_nonroot = (getuid () != 0);
  ret->hdr = g_steal_pointer (&hdr);
  ret->cpio_offset = cpio_offset;
  ret->pkg = pkg ? g_object_ref (pkg) : NULL;
  ret->opt_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (flags & RPMOSTREE_IMPORTER_FLAGS_NODOCS)
    ret->doc_files = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, NULL);
  build_rpmfi_overrides (ret);

 out:
  if (archive)
    archive_read_free (archive);
  if (fi)
    rpmfiFree (fi);
  return ret;
}

void
rpmostree_importer_set_rojig_mode (RpmOstreeImporter                    *self,
                                   GVariant *xattr_table,
                                   GVariant *xattrs)
{
  self->rojig_mode = TRUE;
  self->rojig_xattr_table = g_variant_ref (xattr_table);
  g_variant_get (xattrs, "(s@a(su))",
                 &self->rojig_cacheid,
                 &self->rojig_xattrs);
}

static void
get_rpmfi_override (RpmOstreeImporter *self,
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
rpmostree_importer_get_ostree_branch (RpmOstreeImporter *self)
{
  if (!self->ostree_branch)
    {
      if (self->rojig_mode)
        self->ostree_branch = rpmostree_get_rojig_branch_header (self->hdr);
      else
        self->ostree_branch = rpmostree_get_cache_branch_header (self->hdr);
    }

  return self->ostree_branch;
}

static gboolean
get_lead_sig_header_as_bytes (RpmOstreeImporter *self,
                              GBytes  **out_metadata,
                              GCancellable *cancellable,
                              GError  **error)
{
  /* Inline a pread() based reader here to avoid affecting the file
   * offset since both librpm and libarchive have references.
   */
  g_autofree char *buf = g_malloc (self->cpio_offset);
  char *bufp = buf;
  size_t bytes_remaining = self->cpio_offset;
  while (bytes_remaining > 0)
    {
      ssize_t bytes_read = TEMP_FAILURE_RETRY (pread (self->fd, bufp, bytes_remaining, bufp - buf));
      if (bytes_read < 0)
        return glnx_throw_errno_prefix (error, "pread");
      if (bytes_read == 0)
        break;
      bufp += bytes_read;
      bytes_remaining -= bytes_read;
    }
  if (bytes_remaining > 0)
    return glnx_throw (error, "Failed to read %" G_GSIZE_FORMAT " bytes of metadata",
                       bytes_remaining);

  *out_metadata = g_bytes_new_take (g_steal_pointer (&buf), self->cpio_offset);
  return TRUE;
}

/* Generate per-package metadata; for now this is just the id of the repo
 * where it originated, and the timestamp, which we can use for up-to-date
 * checks.  This is a bit like what the `yumdb` in /var/lib/yum
 * does.  See also
 * https://github.com/rpm-software-management/libdnf/pull/199/
 * https://github.com/projectatomic/rpm-ostree/issues/774
 * https://github.com/projectatomic/rpm-ostree/pull/1072
 *
 * Note overlap with rpmostree_context_get_rpmmd_repo_commit_metadata()
 */
static GVariant *
repo_metadata_for_package (DnfRepo *repo)
{
  g_auto(GVariantBuilder) builder;
  g_variant_builder_init (&builder, (GVariantType*)"a{sv}");

  /* For now, just the id...in the future maybe we'll add more, but this is
   * enough to provide useful semantics.
   */
  g_variant_builder_add (&builder, "{sv}",
                         "id", g_variant_new_string (dnf_repo_get_id (repo)));
  g_variant_builder_add (&builder, "{sv}",
                         "timestamp", g_variant_new_uint64 (dnf_repo_get_timestamp_generated (repo)));

  return g_variant_builder_end (&builder);
}

static gboolean
build_metadata_variant (RpmOstreeImporter *self,
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
   *      rpmostree_context_prepare())
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

  /* include basic NEVRA information so we don't have to write out and read back the header
   * just to get e.g. the pkgname */
  g_autofree char *nevra = rpmostree_importer_get_nevra (self);
  g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.nevra",
                         g_variant_new ("(sstsss)", nevra,
                                        headerGetString (self->hdr, RPMTAG_NAME),
                                        headerGetNumber (self->hdr, RPMTAG_EPOCH),
                                        headerGetString (self->hdr, RPMTAG_VERSION),
                                        headerGetString (self->hdr, RPMTAG_RELEASE),
                                        headerGetString (self->hdr, RPMTAG_ARCH)));

  /* The current sepolicy that was used to label the unpacked files is important
   * to record. It will help us during future overlays to determine whether the
   * files should be relabeled. */
  if (self->sepolicy)
    g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.sepolicy",
                           g_variant_new_string
                             (ostree_sepolicy_get_csum (self->sepolicy)));

  /* let's be nice to our future selves just in case */
  g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.unpack_version",
                         g_variant_new_uint32 (1));

  /* Originally we just had unpack_version = 1, let's add a minor version for
   * compatible increments.  Bumped 4 → 5 for timestamp, and 5 → 6 for docs.
   */
  g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.unpack_minor_version",
                         g_variant_new_uint32 (6));

  if (self->pkg)
    {
      DnfRepo *repo = dnf_package_get_repo (self->pkg);
      if (repo)
        {
          g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.repo",
                                 repo_metadata_for_package (repo));
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

  if (self->rojig_mode)
    {
      g_variant_builder_add (&metadata_builder, "{sv}",
                             "rpmostree.rojig",
                             g_variant_new_boolean (TRUE));
      g_variant_builder_add (&metadata_builder, "{sv}",
                             "rpmostree.rojig_cacheid",
                             g_variant_new_string (self->rojig_cacheid));
      g_variant_builder_add (&metadata_builder, "{sv}",
                             "rpmostree.rojig_n_skipped",
                             g_variant_new_uint32 (self->n_rojig_skipped));
      g_variant_builder_add (&metadata_builder, "{sv}",
                             "rpmostree.rojig_total",
                             g_variant_new_uint32 (self->n_rojig_total));

    }

  if (self->doc_files)
    {
      g_variant_builder_add (&metadata_builder, "{sv}",
                             "rpmostree.nodocs",
                             g_variant_new_boolean (TRUE));
    }

  *out_variant = g_variant_builder_end (&metadata_builder);
  return TRUE;
}

typedef struct
{
  RpmOstreeImporter  *self;
  GError  **error;
} cb_data;

/* See this bug:
 * https://bugzilla.redhat.com/show_bug.cgi?id=517575
 */
static void
ensure_directories_user_writable (GFileInfo *file_info)
{
  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
    {
      guint32 mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
      mode |= S_IWUSR;
      g_file_info_set_attribute_uint32 (file_info, "unix::mode", mode);
    }
}

static gboolean
path_for_tmpfiles_should_be_ignored (const char *path)
{
  /* HACK: Avoid generating tmpfiles.d entries for the `rpm` package's
   * /var/lib/rpm entries in --unified-core composes.  A much more
   * rigorous approach here would be to maintain our built-in tmpfiles.d
   * entries as a struct and ensure we're not writing any overrides for
   * those here.
   */
  return g_str_has_prefix (path, "/var/lib/rpm");
}

/* systemd-tmpfiles complains loudly about writing to /var/run; ideally,
 * all of the packages get fixed for this but...eh.
 */
static void
append_translated_tmpfiles_path (GString *buf, const char *path)
{
  static const char varrun[] = "/var/run/";
  if (g_str_has_prefix (path, varrun))
    path += strlen ("/var");
  /* Handle file paths with spaces and other chars https://github.com/coreos/rpm-ostree/issues/2029 */
  g_autofree char *quoted = rpmostree_maybe_shell_quote (path);
  g_string_append (buf, quoted ?: path);
}

static void
append_tmpfiles_d (RpmOstreeImporter *self,
                   const char *path,
                   GFileInfo *finfo,
                   const char *user,
                   const char *group)
{
  if (path_for_tmpfiles_should_be_ignored (path))
    return;

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
  append_translated_tmpfiles_path (tmpfiles_d, path);

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
  return (*path == '\0' || rpmostree_relative_path_is_ostree_compliant (path));
}

static OstreeRepoCommitFilterResult
compose_filter_cb (OstreeRepo         *repo,
                   const char         *path,
                   GFileInfo          *file_info,
                   gpointer            user_data)
{
  RpmOstreeImporter *self = ((cb_data*)user_data)->self;
  GError **error = ((cb_data*)user_data)->error;

  const char *user = NULL;
  const char *group = NULL;

  gboolean error_was_set = (error && *error != NULL);

  /* Are we filtering out docs?  Let's check that first */
  if (self->doc_files && g_hash_table_contains (self->doc_files, path))
    return OSTREE_REPO_COMMIT_FILTER_SKIP;

  /* Lookup any rpmfi overrides (was parsed from the header) */
  get_rpmfi_override (self, path, &user, &group, NULL);

  if (self->unpacking_as_nonroot)
    {
      /* In the unprivileged case, libarchive returns our own uid by default.
       * Let's ensure the object is always owned by 0/0, since we apply rpm
       * header uid/gid at checkout time anyways.
       *
       * Note that for `ex container` we use
       * OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS, which forces
       * this, and that path also doesn't use this function, it uses
       * unprivileged_filter_cb.
       */
      g_file_info_set_attribute_uint32 (file_info, "unix::uid", 0);
      g_file_info_set_attribute_uint32 (file_info, "unix::gid", 0);
    }
  else
    {
      /* sanity check that RPM isn't using CPIO id fields */
      const guint32 uid = g_file_info_get_attribute_uint32 (file_info, "unix::uid");
      const guint32 gid = g_file_info_get_attribute_uint32 (file_info, "unix::gid");
      if (uid != 0 || gid != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "RPM had unexpected non-root owned path \"%s\", marked as %u:%u)", path, uid, gid);
          return OSTREE_REPO_COMMIT_FILTER_SKIP;
        }
    }

  /* Special case exemptions */
  if (g_str_has_prefix (path, "/usr/etc/selinux") &&
      g_str_has_suffix (path, ".LOCK"))
    {
      /* These empty lock files cause problems;
       * https://github.com/projectatomic/rpm-ostree/pull/1002
       */
      return OSTREE_REPO_COMMIT_FILTER_SKIP;
    }
  /* convert /run and /var entries to tmpfiles.d */
  else if (g_str_has_prefix (path, "/run/") ||
           g_str_has_prefix (path, "/var/"))
    {
      /* HACK: Avoid generating tmpfiles.d entries for the `rpm` package's
       * /var/lib/rpm entries in --unified-core composes.  A much more
       * rigorous approach here would be to maintain our built-in tmpfiles.d
       * entries as a struct and ensure we're not writing any overrides for
       * those here.
       */
      if (!g_str_has_prefix (path, "/var/lib/rpm"))
        {
          append_tmpfiles_d (self, path, file_info,
                             user ?: "root", group ?: "root");
        }
      return OSTREE_REPO_COMMIT_FILTER_SKIP;
    }
  else if (!error_was_set)
    {
      /* And ensure the RPM installs into supported paths.
       * Note that we rewrite opt in handle_translate_pathname, but
       * this gets called with the old path, so handle it here too. */
      if (!(path_is_ostree_compliant (path) ||
            g_str_equal (path, "opt") ||
            g_str_has_prefix (path, "opt/")))
        {
          if ((self->flags & RPMOSTREE_IMPORTER_FLAGS_SKIP_EXTRANEOUS) == 0)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Unsupported path: %s; See %s",
                         path, "https://github.com/projectatomic/rpm-ostree/issues/233");
          return OSTREE_REPO_COMMIT_FILTER_SKIP;
        }
    }

  ensure_directories_user_writable (file_info);

  /* See similar code in `ostree commit` - https://github.com/ostreedev/ostree/pull/2091/commits/7392259332e00c33ed45b904deabde08f4da3e3c */
  if ((self->flags & RPMOSTREE_IMPORTER_FLAGS_RO_EXECUTABLES) > 0 &&
      g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      guint32 mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
      if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        {
          mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
          g_file_info_set_attribute_uint32 (file_info, "unix::mode", mode);
        }
    }

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

static OstreeRepoCommitFilterResult
unprivileged_filter_cb (OstreeRepo         *repo,
                        const char         *path,
                        GFileInfo          *file_info,
                        gpointer            user_data)
{
  RpmOstreeImporter *self = ((cb_data*)user_data)->self;
  /* Are we filtering out docs?  Let's check that first */
  if (self->doc_files && g_hash_table_contains (self->doc_files, path))
    return OSTREE_REPO_COMMIT_FILTER_SKIP;

  /* First, the common directory workaround */
  ensure_directories_user_writable (file_info);

  /* For unprivileged unpacks, ensure that all files are at least user-readable.
   * this is (AFAIK) just limited to /usr/etc/{,g}shadow.
   * See also: https://github.com/projectatomic/rpm-ostree/pull/1046
   * AKA commit 334f0b89be271cbe2b9973ebc7eab50f955517e8
   */
  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      guint32 mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
      mode |= S_IRUSR;
      g_file_info_set_attribute_uint32 (file_info, "unix::mode", mode);
    }

  /* HACK: Also special-case rpm's `/var/lib/rpm` here like in the privileged flow;
   * otherwise libsolv can get confused (see
   * https://github.com/projectatomic/rpm-ostree/pull/290) */
  if (g_str_has_prefix (path, "/var/lib/rpm"))
    return OSTREE_REPO_COMMIT_FILTER_SKIP;

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

#ifdef BUILDOPT_ROJIG
static OstreeRepoCommitFilterResult
rojig_filter_cb (OstreeRepo         *repo,
                 const char         *path,
                 GFileInfo          *file_info,
                 gpointer            user_data)
{
  RpmOstreeImporter *self = ((cb_data*)user_data)->self;
  GError **error = ((cb_data*)user_data)->error;
  const gboolean error_was_set = (error && *error != NULL);

  if (error_was_set)
    return OSTREE_REPO_COMMIT_FILTER_SKIP;

  self->n_rojig_total++;

  if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
    {
      self->rojig_next_xattrs = NULL;
      if (!rpmostree_rojig_assembler_xattr_lookup (self->rojig_xattr_table, path,
                                                   self->rojig_xattrs,
                                                   &self->rojig_next_xattrs,
                                                   error))
        return OSTREE_REPO_COMMIT_FILTER_SKIP;
      /* No xattrs means we don't need to import it */
      if (!self->rojig_next_xattrs)
        {
          self->n_rojig_skipped++;
          return OSTREE_REPO_COMMIT_FILTER_SKIP;
        }
    }

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

static GVariant*
rojig_xattr_cb (OstreeRepo  *repo,
                const char  *path,
                GFileInfo   *file_info,
                gpointer     user_data)
{
  RpmOstreeImporter *self = user_data;
  return g_steal_pointer (&self->rojig_next_xattrs);
}
#endif

static GVariant*
xattr_cb (OstreeRepo  *repo,
          const char  *path,
          GFileInfo   *file_info,
          gpointer     user_data)
{
  RpmOstreeImporter *self = user_data;
  const char *fcaps = NULL;

  get_rpmfi_override (self, path, NULL, NULL, &fcaps);

  if (fcaps != NULL && fcaps[0] != '\0')
    return rpmostree_fcap_to_xattr_variant (fcaps);

  return NULL;
}

static char *
get_first_path_element (const char *rel_path)
{
  const char *end = strchr(rel_path, '/');

  if (end == NULL)
    return g_strdup (rel_path);
  else
    return g_strndup (rel_path, end - rel_path);
}

/* Given a path in an RPM archive, possibly translate it
 * for ostree convention.
 */
static char *
handle_translate_pathname (OstreeRepo   *repo,
                           const struct stat *stbuf,
                           const char   *path,
                           gpointer      user_data)
{
  RpmOstreeImporter *self = user_data;

  if (g_str_has_prefix (path, "opt/"))
    g_hash_table_add (self->opt_files,
                      get_first_path_element (path + strlen("opt/")));

  return rpmostree_translate_path_for_ostree (path);
}

static gboolean
import_rpm_to_repo (RpmOstreeImporter *self,
                    char             **out_csum,
                    GCancellable      *cancellable,
                    GError           **error)
{
  OstreeRepo *repo = self->repo;
  /* Passed to the commit modifier */
  GError *cb_error = NULL;
  cb_data fdata = { self, &cb_error };

  OstreeRepoCommitFilter filter;
  /* This logic replaces our old UNPRIVILEGED flag; we now assume bare-user-only
   * is unprivileged, anything else is a compose.
   */
  const gboolean unprivileged = ostree_repo_get_mode (repo) == OSTREE_REPO_MODE_BARE_USER_ONLY;
  if (self->rojig_mode)
    {
#ifdef BUILDOPT_ROJIG
      filter = rojig_filter_cb;
#else
      g_assert_not_reached ();
#endif
    }
  else if (unprivileged)
    filter = unprivileged_filter_cb;
  else
    filter = compose_filter_cb;

  /* If changing this, also look at changing rpmostree-postprocess.c */
  OstreeRepoCommitModifierFlags modifier_flags =
    OSTREE_REPO_COMMIT_MODIFIER_FLAGS_ERROR_ON_UNLABELED;
  if (unprivileged)
    modifier_flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS;
  g_autoptr(OstreeRepoCommitModifier) modifier =
    ostree_repo_commit_modifier_new (modifier_flags, filter, &fdata, NULL);
  if (self->rojig_mode)
    {
#ifdef BUILDOPT_ROJIG
      ostree_repo_commit_modifier_set_xattr_callback (modifier, rojig_xattr_cb,
                                                      NULL, self);
      g_assert (self->sepolicy == NULL);
#else
      g_assert_not_reached ();
#endif
    }
  else
    {
      ostree_repo_commit_modifier_set_xattr_callback (modifier, xattr_cb, NULL, self);
      ostree_repo_commit_modifier_set_sepolicy (modifier, self->sepolicy);
    }

  OstreeRepoImportArchiveOptions opts = { 0 };
  opts.ignore_unsupported_content = TRUE;
  opts.autocreate_parents = TRUE;
  opts.translate_pathname = handle_translate_pathname;
  opts.translate_pathname_user_data = self;

  g_autoptr(OstreeMutableTree) mtree = ostree_mutable_tree_new ();
  if (!ostree_repo_import_archive_to_mtree (repo, &opts, self->archive, mtree,
                                            modifier, cancellable, error))
    return glnx_prefix_error (error, "Importing archive");

  /* check if any of the cbs set an error */
  if (cb_error != NULL)
    {
      g_propagate_error (error, cb_error);
      return FALSE;
    }

  GLNX_HASH_TABLE_FOREACH (self->opt_files, const char*, filename)
    {
      g_autofree char *opt = g_strconcat ("/opt/", filename, NULL);
      g_autofree char *opt_quoted = rpmostree_maybe_shell_quote (opt);
      /* Note that the destination can't be quoted as systemd just
       * parses the remainder of the line, and doesn't expand quotes.
       **/
      g_string_append_printf (self->tmpfiles_d,
                              "L %s - - - - /usr/lib/opt/%s\n",
                              opt_quoted ?: opt, filename);
    }

  /* Handle any data we've accumulated data to write to tmpfiles.d.
   * I originally tried to do this entirely in memory but things
   * like selinux labeling only happen as callbacks out of using
   * the input dfd/archive paths...so let's just use a tempdir. (:sadface:)
   */
  g_auto(GLnxTmpDir) tmpdir = { 0, };
  if (self->tmpfiles_d->len > 0)
    {
      g_autofree char *pkgname = headerGetAsString (self->hdr, RPMTAG_NAME);

      if (!glnx_mkdtemp ("rpm-ostree-import.XXXXXX", 0700, &tmpdir, error))
        return FALSE;
      if (!glnx_shutil_mkdir_p_at (tmpdir.fd, "usr/lib/tmpfiles.d", 0755, cancellable,  error))
        return FALSE;
      if (!glnx_file_replace_contents_at (tmpdir.fd, glnx_strjoina ("usr/lib/tmpfiles.d/", "pkg-", pkgname, ".conf"),
                                          (guint8*)self->tmpfiles_d->str, self->tmpfiles_d->len, GLNX_FILE_REPLACE_NODATASYNC,
                                          cancellable, error))
        return FALSE;

      if (!ostree_repo_write_dfd_to_mtree (repo, tmpdir.fd, ".", mtree, modifier,
                                           cancellable, error))
        return glnx_prefix_error (error, "Writing tmpfiles mtree");

      /* check if any of the cbs set an error */
      if (cb_error != NULL)
        {
          g_propagate_error (error, cb_error);
          return FALSE;
        }
    }

  g_autoptr(GFile) root = NULL;
  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    return glnx_prefix_error (error, "Writing mtree");

  g_autoptr(GVariant) metadata = NULL;
  if (!build_metadata_variant (self, &metadata, cancellable, error))
    return FALSE;
  g_variant_ref_sink (metadata);

  /* Use the build timestamp for the commit: this ensures that committing the
   * same RPM always yields the same checksum, which is a useful property to
   * have (barring changes in the unpacker, in which case we wouldn't want the
   * same checksum anyway). */
  guint64 buildtime = headerGetNumber (self->hdr, RPMTAG_BUILDTIME);

  if (!ostree_repo_write_commit_with_time (repo, NULL, "", "", metadata,
                                           OSTREE_REPO_FILE (root), buildtime,
                                           out_csum, cancellable, error))
    return glnx_prefix_error (error, "Writing commit");

  return TRUE;
}

gboolean
rpmostree_importer_run (RpmOstreeImporter *self,
                        char             **out_csum,
                        GCancellable      *cancellable,
                        GError           **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_autofree char *csum = NULL;
  if (!import_rpm_to_repo (self, &csum, cancellable, error))
    {
      g_autofree char *name = headerGetAsString (self->hdr, RPMTAG_NAME);
      return glnx_prefix_error (error, "Importing package '%s'", name);
    }

  const char *branch = rpmostree_importer_get_ostree_branch (self);
  ostree_repo_transaction_set_ref (self->repo, NULL, branch, csum);

  if (out_csum)
    *out_csum = g_steal_pointer (&csum);
  return TRUE;
}

static void
import_in_thread (GTask            *task,
                  gpointer          source,
                  gpointer          task_data,
                  GCancellable     *cancellable)
{
  GError *local_error = NULL;
  RpmOstreeImporter *self = source;
  g_autofree char *rev = NULL;

  if (!rpmostree_importer_run (self, &rev, cancellable, &local_error))
    g_task_return_error (task, local_error);
  else
    g_task_return_pointer (task, g_steal_pointer (&rev), g_free);
}

void
rpmostree_importer_run_async (RpmOstreeImporter  *self,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data)
{
  g_autoptr(GTask) task = g_task_new (self, cancellable, callback, user_data);
  g_task_run_in_thread (task, import_in_thread);
}

char *
rpmostree_importer_run_async_finish (RpmOstreeImporter  *self,
                                     GAsyncResult       *result,
                                     GError            **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  return g_task_propagate_pointer ((GTask*)result, error);
}

char *
rpmostree_importer_get_nevra (RpmOstreeImporter *self)
{
  if (self->hdr == NULL)
    return NULL;
  return rpmostree_header_custom_nevra_strdup (self->hdr,
                                               PKG_NEVRA_FLAGS_NAME |
                                               PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE |
                                               PKG_NEVRA_FLAGS_ARCH);
}

const char *
rpmostree_importer_get_header_sha256 (RpmOstreeImporter *self)
{
  return self->hdr_sha256;
}


static gboolean
import_local_rpm (OstreeRepo    *repo,
                  int           *fd,
                  char         **sha256_nevra,
                  GCancellable  *cancellable,
                  GError       **error)
{
  /* let's just use the current sepolicy -- we'll just relabel it if the new
   * base turns out to have a different one */
  glnx_autofd int rootfs_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &rootfs_dfd, error))
    return FALSE;
  g_autoptr(OstreeSePolicy) policy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
  if (policy == NULL)
    return FALSE;

  g_autoptr(RpmOstreeImporter) unpacker = rpmostree_importer_new_take_fd (fd, repo, NULL, 0, policy, error);
  if (unpacker == NULL)
    return FALSE;

  if (!rpmostree_importer_run (unpacker, NULL, cancellable, error))
    return FALSE;

  g_autofree char *nevra = rpmostree_importer_get_nevra (unpacker);
  *sha256_nevra = g_strconcat (rpmostree_importer_get_header_sha256 (unpacker),
                               ":", nevra, NULL);

  return TRUE;
}

static void
ptr_close_fd (gpointer fdp)
{
  int fd = GPOINTER_TO_INT (fdp);
  glnx_close_fd (&fd);
}

/* GUnixFDList doesn't allow stealing individual members */
static GPtrArray *
unixfdlist_to_ptrarray (GUnixFDList *fdl)
{
  gint len;
  gint *fds = g_unix_fd_list_steal_fds (fdl, &len);
  GPtrArray *ret = g_ptr_array_new_with_free_func ((GDestroyNotify)ptr_close_fd);
  for (int i = 0; i < len; i++)
    g_ptr_array_add (ret, GINT_TO_POINTER (fds[i]));
  return ret;
}

gboolean
rpmostree_importer_import_many_local_rpms (OstreeRepo    *repo,
                        GUnixFDList   *fdl,
                        GPtrArray    **out_pkgs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  /* Note that we record the SHA-256 of the RPM header in the origin to make sure that e.g.
   * if we somehow re-import the same NEVRA with different content, we error out. We don't
   * record the checksum of the branch itself, because it may need relabeling and that's OK.
   * */

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  /* Note use of commit-on-failure */
  if (!rpmostree_repo_auto_transaction_start (&txn, repo, TRUE, cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) pkgs = g_ptr_array_new_with_free_func (g_free);

  g_autoptr(GPtrArray) fds = unixfdlist_to_ptrarray (fdl);
  for (guint i = 0; i < fds->len; i++)
    {
      /* Steal fd from the ptrarray */
      glnx_autofd int fd = GPOINTER_TO_INT (fds->pdata[i]);
      fds->pdata[i] = GINT_TO_POINTER (-1);
      g_autofree char *sha256_nevra = NULL;
      /* Transfer fd to import */
      if (!import_local_rpm (repo, &fd, &sha256_nevra, cancellable, error))
        return FALSE;

      g_ptr_array_add (pkgs, g_steal_pointer (&sha256_nevra));
    }

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;
  txn.initialized = FALSE;

  *out_pkgs = g_steal_pointer (&pkgs);
  return TRUE;
}
