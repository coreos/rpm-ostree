/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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

#include "string.h"

#include <ostree.h>
#include <errno.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <utime.h>
#include <err.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <libglnx.h>
#include <stdlib.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include "rpmostree-postprocess.h"
#include "rpmostree-kernel.h"
#include "rpmostree-bwrap.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-util.h"

typedef enum {
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_BOTH,
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW
} RpmOstreePostprocessBootLocation;

/* This bwrap case is for treecompose which isn't yet operating on
 * hardlinks, so we just bind mount things mutably.
 */
static gboolean
run_bwrap_mutably (int           rootfs_fd,
                   const char   *binpath,
                   char        **child_argv,
                   GCancellable *cancellable,
                   GError      **error)
{
  struct stat stbuf;
  const char *etc_bind;

  /* This gets called both by treecompose, where in the non-unified path we just
   * have /etc, and in kernel postprocessing where we have usr/etc.
   */
  if (!glnx_fstatat_allow_noent (rootfs_fd, "etc", &stbuf, 0, error))
    return FALSE;
  if (errno == ENOENT)
    etc_bind = "usr/etc";
  else
    etc_bind = "etc";

  g_autoptr(RpmOstreeBwrap) bwrap =
    rpmostree_bwrap_new (rootfs_fd, RPMOSTREE_BWRAP_MUTATE_FREELY, error,
                         "--bind", "var", "/var",
                         "--bind", etc_bind, "/etc",
                         NULL);
  if (!bwrap)
    return FALSE;

  rpmostree_bwrap_append_child_argv (bwrap, binpath, NULL);

  /* https://github.com/projectatomic/bubblewrap/issues/91 */
  { gboolean first = TRUE;
    for (char **iter = child_argv; iter && *iter; iter++)
      {
        if (first)
          first = FALSE;
        else
          rpmostree_bwrap_append_child_argv (bwrap, *iter, NULL);
      }
  }

  if (!rpmostree_bwrap_run (bwrap, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
rename_if_exists (int         src_dfd,
                  const char *from,
                  int         dest_dfd,
                  const char *to,
                  GError    **error)
{
  struct stat stbuf;
  const char *errmsg = glnx_strjoina ("renaming ", from);
  GLNX_AUTO_PREFIX_ERROR (errmsg, error);

  if (!glnx_fstatat_allow_noent (src_dfd, from, &stbuf, 0, error))
    return FALSE;
  if (errno == 0)
    {
      if (renameat (src_dfd, from, dest_dfd, to) < 0)
        {
          /* Handle empty directory in legacy location */
          if (errno == EEXIST)
            {
              if (!glnx_unlinkat (src_dfd, from, AT_REMOVEDIR, error))
                return FALSE;
            }
          else
            return glnx_throw_errno_prefix (error, "renameat(%s)", to);
        }
    }

  return TRUE;
}

typedef struct {
  const char *target;
  const char *src;
} Symlink;

/* Initialize deployment root directory. This is mostly hardcoded; in the future
 * we may make things more configurable.
 */
static gboolean
init_rootfs (int            dfd,
             gboolean       tmp_is_dir,
             GCancellable  *cancellable,
             GError       **error)
{
  const char *toplevel_dirs[] = { "dev", "proc", "run", "sys", "var", "sysroot" };
  const Symlink symlinks[] = {
    { "var/opt", "opt" },
    { "var/srv", "srv" },
    { "var/mnt", "mnt" },
    { "var/roothome", "root" },
    { "var/home", "home" },
    { "run/media", "media" },
    { "sysroot/ostree", "ostree" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (toplevel_dirs); i++)
    {
      if (!glnx_ensure_dir (dfd, toplevel_dirs[i], 0755, error))
        return FALSE;
    }

  for (guint i = 0; i < G_N_ELEMENTS (symlinks); i++)
    {
      const Symlink*linkinfo = symlinks + i;
      if (symlinkat (linkinfo->target, dfd, linkinfo->src) < 0)
        return glnx_throw_errno_prefix (error, "symlinkat");
    }

  if (tmp_is_dir)
    {
      if (!glnx_shutil_mkdir_p_at (dfd, "tmp", 01777,
                                   cancellable, error))
        return FALSE;
      if (fchmodat (dfd, "tmp", 01777, 0) == -1)
        return glnx_throw_errno_prefix (error, "fchmodat");
    }
  else
    {
      if (symlinkat ("sysroot/tmp", dfd, "tmp") < 0)
        return glnx_throw_errno_prefix (error, "symlinkat");
    }

  return TRUE;
}

/* Given a directory referenced by @src_dfd+@src_path,
 * Create @dest_dfd+@dest_path as a directory, hardlinking
 * all content - recursively.
 */
static gboolean
hardlink_recurse (int                src_dfd,
                  const char        *src_path,
                  int                dest_dfd,
                  const char        *dest_path,
                  GCancellable      *cancellable,
                  GError            **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  glnx_fd_close int dest_target_dfd = -1;

  if (!glnx_dirfd_iterator_init_at (src_dfd, src_path, TRUE, &dfd_iter, error))
    return FALSE;

  if (!glnx_opendirat (dest_dfd, dest_path, TRUE, &dest_target_dfd, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      if (!glnx_fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;

      if (dent->d_type == DT_DIR)
        {
          mode_t perms = stbuf.st_mode & ~S_IFMT;

          if (!glnx_ensure_dir (dest_target_dfd, dent->d_name, perms, error))
            return FALSE;
          if (fchmodat (dest_target_dfd, dent->d_name, perms, 0) < 0)
            return glnx_throw_errno_prefix (error, "fchmodat");
          if (!hardlink_recurse (dfd_iter.fd, dent->d_name,
                                 dest_target_dfd, dent->d_name,
                                 cancellable, error))
            return FALSE;
        }
      else
        {
          if (linkat (dfd_iter.fd, dent->d_name,
                      dest_target_dfd, dent->d_name, 0) < 0)
            return glnx_throw_errno_prefix (error, "linkat");
        }
    }

  return TRUE;
}

/* Handle the kernel/initramfs, which can be in at least 2 different places:
 *  - /boot (CentOS, Fedora treecompose before we suppressed kernel.spec's %posttrans)
 *  - /usr/lib/modules (Fedora treecompose without kernel.spec's %posttrans)
 *
 * We then need to handle the boot_location option, which can put that data in
 * either both (/boot and /usr/lib/ostree-boot), or just the latter.
 */
static gboolean
process_kernel_and_initramfs (int            rootfs_dfd,
                              JsonObject    *treefile,
                              GCancellable  *cancellable,
                              GError       **error)
{
  /* The current systemd kernel-install will inject
   * /boot/${machine_id}/${uname -r} which we don't use;
   * to avoid confusion, we will delete it.  This relies
   * on systemd itself having set up the machine id from its %post,
   * so we need to read it.  We'll reset the machine ID after this.
   */
  { glnx_fd_close int fd = openat (rootfs_dfd, "usr/etc/machine-id", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      {
        if (errno != ENOENT)
          return glnx_throw_errno_prefix (error, "openat(usr/etc/machine-id)");
      }
    else
      {
        g_autofree char *old_machine_id = glnx_fd_readall_utf8 (fd, NULL, cancellable, error);
        if (!old_machine_id)
          return FALSE;
        if (strlen (old_machine_id) != 33)
          return glnx_throw (error, "invalid machine ID '%.33s'", old_machine_id);
        /* Trim newline */
        old_machine_id[32] = '\0';

        const char *boot_machineid_dir = glnx_strjoina ("boot/", old_machine_id);
        if (!glnx_shutil_rm_rf_at (rootfs_dfd, boot_machineid_dir, cancellable, error))
          return FALSE;
      }
  }

  /* We need to move non-kernel data (bootloader bits usually) into
   * /usr/lib/ostree-boot; this will also take care of moving the kernel in legacy
   * paths (CentOS, Fedora <= 24), etc.
   */
  if (!rename_if_exists (rootfs_dfd, "boot", rootfs_dfd, "usr/lib/ostree-boot", error))
    return FALSE;

  /* Find the kernel in the source root (at this point one of usr/lib/modules or
   * usr/lib/ostree-boot)
   */
  g_autoptr(GVariant) kernelstate = rpmostree_find_kernel (rootfs_dfd, cancellable, error);
  if (!kernelstate)
    return FALSE;
  const char* kernel_path;
  const char* initramfs_path;
  const char *kver;
  const char *bootdir;
  /* Used to optionally hardlink result of our dracut run */
  g_variant_get (kernelstate, "(&s&s&sm&s)",
                 &kver, &bootdir,
                 &kernel_path, &initramfs_path);

  /* We generate our own initramfs with custom arguments, so if the RPM install
   * generated one (should only happen on CentOS now), delete it.
   */
  if (initramfs_path)
    {
      g_assert_cmpstr (bootdir, ==, "usr/lib/ostree-boot");
      g_assert_cmpint (*initramfs_path, !=, '/');
      g_print ("Removing RPM-generated '%s'\n", initramfs_path);
      if (!glnx_shutil_rm_rf_at (rootfs_dfd, initramfs_path, cancellable, error))
        return FALSE;
      initramfs_path = NULL;
    }

  /* Ensure depmod (kernel modules index) is up to date; because on Fedora we
   * suppress the kernel %posttrans we need to take care of this.
   */
  {
    char *child_argv[] = { "depmod", (char*)kver, NULL };
    if (!run_bwrap_mutably (rootfs_dfd, "depmod", child_argv, cancellable, error))
      return FALSE;
  }

  RpmOstreePostprocessBootLocation boot_location =
    RPMOSTREE_POSTPROCESS_BOOT_LOCATION_BOTH;
  const char *boot_location_str = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile,
                                                              "boot_location",
                                                              &boot_location_str, error))
    return FALSE;
  if (boot_location_str != NULL)
    {
      /* Make "legacy" an alias for "both" */
      if (strcmp (boot_location_str, "both") == 0 ||
          strcmp (boot_location_str, "legacy") == 0)
        ;
      else if (strcmp (boot_location_str, "new") == 0)
        boot_location = RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW;
      else
        return glnx_throw (error, "Invalid boot location '%s'", boot_location_str);
    }

  /* Ensure the /etc/machine-id file is present and empty; it is read by
   * dracut. Apparently systemd doesn't work when the file is missing (as of
   * systemd-219-9.fc22) but it is correctly populated if the file is there.
   */
  g_print ("Creating empty machine-id\n");
  if (!glnx_file_replace_contents_at (rootfs_dfd, "usr/etc/machine-id", (guint8*)"", 0,
                                      GLNX_FILE_REPLACE_NODATASYNC,
                                      cancellable, error))
    return FALSE;

  /* Run dracut with our chosen arguments (commonly at least --no-hostonly) */
  g_autoptr(GPtrArray) dracut_argv = g_ptr_array_new ();
  if (json_object_has_member (treefile, "initramfs-args"))
    {
      JsonArray *initramfs_args = json_object_get_array_member (treefile, "initramfs-args");
      guint len = json_array_get_length (initramfs_args);

      for (guint i = 0; i < len; i++)
        {
          const char *arg = _rpmostree_jsonutil_array_require_string_element (initramfs_args, i, error);
          if (!arg)
            return FALSE;
          g_ptr_array_add (dracut_argv, (char*)arg);
        }
    }
  g_ptr_array_add (dracut_argv, NULL);

  g_auto(GLnxTmpfile) initramfs_tmpf = { 0, };
  if (!rpmostree_run_dracut (rootfs_dfd,
                             (const char *const*)dracut_argv->pdata, kver,
                             NULL, &initramfs_tmpf,
                             cancellable, error))
    return FALSE;

  /* We always tell rpmostree_finalize_kernel() to skip /boot, since we'll do a
   * full hardlink pass if needed after that for the kernel + bootloader data.
   */
  if (!rpmostree_finalize_kernel (rootfs_dfd, bootdir, kver, kernel_path,
                                  &initramfs_tmpf,
                                  RPMOSTREE_FINALIZE_KERNEL_USRLIB_OSTREEBOOT,
                                  cancellable, error))
    return FALSE;

  /* We always ensure this exists as a mountpoint */
  if (!glnx_ensure_dir (rootfs_dfd, "boot", 0755, error))
    return FALSE;

  /* If the boot location includes /boot, we also need to copy /usr/lib/ostree-boot there */
  switch (boot_location)
    {
    case RPMOSTREE_POSTPROCESS_BOOT_LOCATION_BOTH:
      {
        g_print ("Using boot location: both\n");
        /* Hardlink the existing content, only a little ugly as
         * we'll end up sha256'ing it twice, but oh well. */
        if (!hardlink_recurse (rootfs_dfd, "usr/lib/ostree-boot",
                               rootfs_dfd, "boot",
                               cancellable, error))
          return glnx_prefix_error (error, "hardlinking /boot");
      }
      break;
    case RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW:
      break;
    }

  return TRUE;
}

static gboolean
convert_var_to_tmpfiles_d_recurse (GOutputStream *tmpfiles_out,
                                   int            dfd,
                                   GString       *prefix,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gsize bytes_written;

  if (!glnx_dirfd_iterator_init_at (dfd, prefix->str + 1, TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      char filetype_c;
      switch (dent->d_type)
        {
        case DT_DIR:
          filetype_c = 'd';
          break;
        case DT_LNK:
          filetype_c = 'L';
          break;
        default:
          g_print ("Ignoring non-directory/non-symlink '%s/%s'\n",
                   prefix->str,
                   dent->d_name);
          if (!glnx_unlinkat (dfd_iter.fd, dent->d_name, 0, error))
            return FALSE;
          continue;
        }

      g_autoptr(GString) tmpfiles_d_buf = g_string_new ("");
      g_string_append_c (tmpfiles_d_buf, filetype_c);
      g_string_append_c (tmpfiles_d_buf, ' ');
      g_string_append (tmpfiles_d_buf, prefix->str);
      g_string_append_c (tmpfiles_d_buf, '/');
      g_string_append (tmpfiles_d_buf, dent->d_name);

      if (filetype_c == 'd')
        {
          struct stat stbuf;
          if (!glnx_fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
            return FALSE;

          g_string_append_printf (tmpfiles_d_buf, " 0%02o", stbuf.st_mode & ~S_IFMT);
          g_string_append_printf (tmpfiles_d_buf, " %d %d - -", stbuf.st_uid, stbuf.st_gid);

          /* Push prefix */
          g_string_append_c (prefix, '/');
          g_string_append (prefix, dent->d_name);

          if (!convert_var_to_tmpfiles_d_recurse (tmpfiles_out, dfd, prefix,
                                                  cancellable, error))
            return FALSE;

          /* Pop prefix */
          {
            char *r = memrchr (prefix->str, '/', prefix->len);
            g_assert (r != NULL);
            g_string_truncate (prefix, r - prefix->str);
          }
        }
      else
        {
          g_autofree char *link = glnx_readlinkat_malloc (dfd_iter.fd, dent->d_name, cancellable, error);
          if (!link)
            return FALSE;
          g_string_append (tmpfiles_d_buf, " - - - - ");
          g_string_append (tmpfiles_d_buf, link);

        }

      if (!glnx_unlinkat (dfd_iter.fd, dent->d_name,
                          dent->d_type == DT_DIR ? AT_REMOVEDIR : 0, error))
        return FALSE;

      g_string_append_c (tmpfiles_d_buf, '\n');

      if (!g_output_stream_write_all (tmpfiles_out, tmpfiles_d_buf->str,
                                      tmpfiles_d_buf->len, &bytes_written,
                                      cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
convert_var_to_tmpfiles_d (int            src_rootfs_dfd,
                           int            dest_rootfs_dfd,
                           GCancellable  *cancellable,
                           GError       **error)
{
  glnx_fd_close int var_dfd = -1;
  /* List of files that are known to possibly exist, but in practice
   * things work fine if we simply ignore them.  Don't add something
   * to this list unless you've verified it's handled correctly at
   * runtime.  (And really both in CentOS and Fedora)
   */
  const char *known_state_files[] = {
    "lib/systemd/random-seed", /* https://bugzilla.redhat.com/show_bug.cgi?id=789407 */
    "lib/systemd/catalog/database",
    "lib/plymouth/boot-duration",
    "log/wtmp", /* These two are part of systemd's var.tmp */
    "log/btmp",
  };

  if (!glnx_opendirat (src_rootfs_dfd, "var", TRUE, &var_dfd, error))
    return FALSE;

  /* We never want to traverse into /run when making tmpfiles since it's a tmpfs */
  /* Note that in a Fedora root, /var/run is a symlink, though on el7, it can be a dir.
   * See: https://github.com/projectatomic/rpm-ostree/pull/831 */
  if (!glnx_shutil_rm_rf_at (var_dfd, "run", cancellable, error))
    return FALSE;

  /* Here, delete some files ahead of time to avoid emitting warnings
   * for things that are known to be harmless.
   */
  for (guint i = 0; i < G_N_ELEMENTS (known_state_files); i++)
    {
      const char *path = known_state_files[i];
      if (unlinkat (var_dfd, path, 0) < 0)
        {
          if (errno != ENOENT)
            return glnx_throw_errno_prefix (error, "unlinkat(%s)", path);
        }
    }

  /* Convert /var wholesale to tmpfiles.d. Note that with unified core, this
   * code should no longer be necessary as we convert packages on import.
   */
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (dest_rootfs_dfd, "usr/lib/tmpfiles.d", O_WRONLY | O_CLOEXEC,
                                      &tmpf, error))
    return FALSE;
  g_autoptr(GOutputStream) tmpfiles_out = g_unix_output_stream_new (tmpf.fd, FALSE);
  if (!tmpfiles_out)
    return FALSE;

  g_autoptr(GString) prefix = g_string_new ("/var");
  if (!convert_var_to_tmpfiles_d_recurse (tmpfiles_out, src_rootfs_dfd, prefix, cancellable, error))
    return FALSE;

  if (!g_output_stream_close (tmpfiles_out, cancellable, error))
    return FALSE;

  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE,
                             dest_rootfs_dfd, "usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf",
                             error))
    return FALSE;

  return TRUE;
}

/* SELinux uses PCRE pre-compiled regexps for binary caches, which can
 * fail if the version of PCRE on the host differs from the version
 * which generated the cache (in the target root).
 *
 * Note also this function is probably already broken in Fedora
 * 23+ from https://bugzilla.redhat.com/show_bug.cgi?id=1265406
 */
static gboolean
workaround_selinux_cross_labeling_recurse (int            dfd,
                                           const char    *path,
                                           GCancellable  *cancellable,
                                           GError       **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (dfd, path, TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      const char *name;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;

      if (!dent)
        break;

      name = dent->d_name;

      if (dent->d_type == DT_DIR)
        {
          if (!workaround_selinux_cross_labeling_recurse (dfd_iter.fd, name, cancellable, error))
            return FALSE;
        }
      else if (g_str_has_suffix (name, ".bin"))
        {
          struct stat stbuf;
          const char *lastdot;
          g_autofree char *nonbin_name = NULL;

          if (!glnx_fstatat (dfd_iter.fd, name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
            return FALSE;

          lastdot = strrchr (name, '.');
          g_assert (lastdot);

          nonbin_name = g_strndup (name, lastdot - name);

          if (TEMP_FAILURE_RETRY (utimensat (dfd_iter.fd, nonbin_name, NULL, 0)) == -1)
            return glnx_throw_errno_prefix (error, "utimensat");
        }
    }

  return TRUE;
}

gboolean
rpmostree_prepare_rootfs_get_sepolicy (int            dfd,
                                       OstreeSePolicy **out_sepolicy,
                                       GCancellable  *cancellable,
                                       GError       **error)
{
  struct stat stbuf;
  const char *policy_path;

  /* Handle the policy being in both /usr/etc and /etc since
   * this function can be called at different points.
   */
  if (!glnx_fstatat_allow_noent (dfd, "usr/etc", &stbuf, 0, error))
    return FALSE;
  if (errno == ENOENT)
    policy_path = "etc/selinux";
  else
    policy_path = "usr/etc/selinux";

  if (!glnx_fstatat_allow_noent (dfd, policy_path, &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == 0)
    {
      if (!workaround_selinux_cross_labeling_recurse (dfd, policy_path,
                                                      cancellable, error))
        return FALSE;
    }

  g_autoptr(OstreeSePolicy) ret_sepolicy = ostree_sepolicy_new_at (dfd, cancellable, error);
  if (ret_sepolicy == NULL)
    return FALSE;

  *out_sepolicy = g_steal_pointer (&ret_sepolicy);
  return TRUE;
}

static char *
replace_nsswitch_string (const char *buf,
                         GError    **error)
{
  gboolean is_passwd;
  gboolean is_group;

  is_passwd = g_str_has_prefix (buf, "passwd:");
  is_group = g_str_has_prefix (buf, "group:");

  if (!(is_passwd || is_group))
    return g_strdup (buf);

  const char *colon = strchr (buf, ':');
  g_assert (colon);

  g_autoptr(GString) retbuf = g_string_new ("");
  /* Insert the prefix */
  g_string_append_len (retbuf, buf, (colon - buf) + 1);

  /* Now parse the elements and try to insert `altfiles`
   * after `files`.
   */
  g_auto(GStrv) elts = g_strsplit_set (colon + 1, " \t", -1);
  gboolean inserted = FALSE;
  for (char **iter = elts; iter && *iter; iter++)
    {
      const char *v = *iter;
      if (!*v)
        continue;
      /* Already have altfiles?  We're done */
      if (strcmp (v, "altfiles") == 0)
        return g_strdup (buf);
      /* We prefer `files altfiles` */
      else if (!inserted && strcmp (v, "files") == 0)
        {
          g_string_append (retbuf, " files altfiles");
          inserted = TRUE;
        }
      else
        {
          g_string_append_c (retbuf, ' ');
          g_string_append (retbuf, v);
        }
    }
  /* Last ditch effort if we didn't find `files` */
  if (!inserted)
    g_string_append (retbuf, " altfiles");
  return g_string_free (g_steal_pointer (&retbuf), FALSE);
}

char *
rpmostree_postprocess_replace_nsswitch (const char *buf,
                                        GError    **error)
{
  g_autoptr(GString) new_buf = g_string_new ("");

  g_auto(GStrv) lines = g_strsplit (buf, "\n", -1);
  for (char **iter = lines; iter && *iter; iter++)
    {
      const char *line = *iter;
      g_autofree char *replaced_line = replace_nsswitch_string (line, error);
      if (!replaced_line)
        return NULL;
      g_string_append (new_buf, replaced_line);
      if (*(iter+1))
        g_string_append_c (new_buf, '\n');
    }
  return g_string_free (g_steal_pointer (&new_buf), FALSE);
}


static gboolean
replace_nsswitch (int            dfd,
                  GCancellable  *cancellable,
                  GError       **error)
{
  g_autofree char *nsswitch_contents =
    glnx_file_get_contents_utf8_at (dfd, "usr/etc/nsswitch.conf", NULL,
                                    cancellable, error);
  if (!nsswitch_contents)
    return FALSE;

  g_autofree char *new_nsswitch_contents =
    rpmostree_postprocess_replace_nsswitch (nsswitch_contents, error);
  if (!new_nsswitch_contents)
    return FALSE;

  if (!glnx_file_replace_contents_at (dfd, "usr/etc/nsswitch.conf",
                                      (guint8*)new_nsswitch_contents, -1,
                                      GLNX_FILE_REPLACE_NODATASYNC,
                                      cancellable, error))
    return FALSE;

  return TRUE;
}

/* SELinux in Fedora >= 24: https://bugzilla.redhat.com/show_bug.cgi?id=1290659 */
static gboolean
postprocess_selinux_policy_store_location (int rootfs_dfd,
                                           GCancellable *cancellable,
                                           GError **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;
  const char *var_policy_location = NULL;
  const char *etc_policy_location = NULL;
  struct stat stbuf;
  const char *name;
  glnx_fd_close int etc_selinux_dfd = -1;

  if (!rpmostree_prepare_rootfs_get_sepolicy (rootfs_dfd, &sepolicy, cancellable, error))
    return FALSE;

  name = ostree_sepolicy_get_name (sepolicy);
  if (!name) /* If there's no policy, shortcut here */
    return TRUE;

  var_policy_location = glnx_strjoina ("var/lib/selinux/", name);
  const char *modules_location = glnx_strjoina (var_policy_location, "/active/modules");
  if (!glnx_fstatat_allow_noent (rootfs_dfd, modules_location, &stbuf, 0, error))
    return FALSE;
  if (errno == ENOENT)
    {
      /* Okay, this is probably CentOS 7, or maybe we have a build of
       * selinux-policy with the path moved back into /etc (or maybe it's
       * in /usr).
       */
      return TRUE;
    }
  g_print ("SELinux policy in /var, enabling workaround\n");

  { g_autofree char *orig_contents = NULL;
    g_autofree char *contents = NULL;
    const char *semanage_path = "usr/etc/selinux/semanage.conf";

    orig_contents = glnx_file_get_contents_utf8_at (rootfs_dfd, semanage_path, NULL,
                                                    cancellable, error);
    if (orig_contents == NULL)
      return glnx_prefix_error (error, "Opening %s", semanage_path);

    contents = g_strconcat (orig_contents, "\nstore-root=/etc/selinux\n", NULL);

    if (!glnx_file_replace_contents_at (rootfs_dfd, semanage_path,
                                        (guint8*)contents, -1, 0,
                                        cancellable, error))
      return glnx_prefix_error (error, "Replacing %s", semanage_path);
  }

  etc_policy_location = glnx_strjoina ("usr/etc/selinux/", name);
  if (!glnx_opendirat (rootfs_dfd, etc_policy_location, TRUE, &etc_selinux_dfd, error))
    return FALSE;

  if (!glnx_dirfd_iterator_init_at (rootfs_dfd, var_policy_location, TRUE, &dfd_iter, error))
    return FALSE;

  /* We take all of the contents of the directory, but not the directory itself */
  while (TRUE)
    {
      struct dirent *dent = NULL;
      const char *name;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      name = dent->d_name;
      if (!glnx_renameat (dfd_iter.fd, name, etc_selinux_dfd, name, error))
        return FALSE;
    }

  return TRUE;
}

/* Prepare a root filesystem, taking mainly the contents of /usr from pkgroot */
static gboolean
create_rootfs_from_pkgroot_content (int            target_root_dfd,
                                    int            src_rootfs_fd,
                                    JsonObject    *treefile,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  gboolean selinux = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile,
                                                               "selinux",
                                                               &selinux,
                                                               error))
    return FALSE;

  gboolean container = FALSE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile,
                                                               "container",
                                                               &container,
                                                               error))
    return FALSE;

  /* Initialize target root */
  g_print ("Initializing rootfs\n");
  gboolean tmp_is_dir = FALSE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile,
                                                               "tmp-is-dir",
                                                               &tmp_is_dir,
                                                               error))
    return FALSE;

  if (!init_rootfs (target_root_dfd, tmp_is_dir, cancellable, error))
    return FALSE;

  g_print ("Migrating /etc/passwd to /usr/lib/\n");
  if (!rpmostree_passwd_migrate_except_root (src_rootfs_fd, RPM_OSTREE_PASSWD_MIGRATE_PASSWD, NULL,
                                             cancellable, error))
    return FALSE;

  g_autoptr(GHashTable) preserve_groups_set = NULL;
  if (json_object_has_member (treefile, "etc-group-members"))
    {
      JsonArray *etc_group_members = json_object_get_array_member (treefile, "etc-group-members");
      preserve_groups_set = _rpmostree_jsonutil_jsarray_strings_to_set (etc_group_members);
    }

  g_print ("Migrating /etc/group to /usr/lib/\n");
  if (!rpmostree_passwd_migrate_except_root (src_rootfs_fd, RPM_OSTREE_PASSWD_MIGRATE_GROUP,
                                             preserve_groups_set,
                                             cancellable, error))
    return FALSE;

  /* NSS configuration to look at the new files */
  if (!replace_nsswitch (src_rootfs_fd, cancellable, error))
    return glnx_prefix_error (error, "nsswitch replacement");

  if (selinux)
    {
      if (!postprocess_selinux_policy_store_location (src_rootfs_fd, cancellable, error))
        return glnx_prefix_error (error, "SELinux postprocess");
    }

  /* We take /usr from the yum content */
  g_print ("Moving /usr to target\n");
  if (!glnx_renameat (src_rootfs_fd, "usr", target_root_dfd, "usr", error))
    return FALSE;

  if (!rpmostree_rootfs_prepare_links (target_root_dfd, cancellable, error))
    return FALSE;
  if (!rpmostree_rootfs_postprocess_common (target_root_dfd, cancellable, error))
    return FALSE;

  if (!convert_var_to_tmpfiles_d (src_rootfs_fd, target_root_dfd, cancellable, error))
    return FALSE;

  /* Also carry along toplevel compat links */
  g_print ("Copying toplevel compat symlinks\n");
  {
    guint i;
    const char *toplevel_links[] = { "lib", "lib64", "lib32",
                                     "bin", "sbin" };
    for (i = 0; i < G_N_ELEMENTS (toplevel_links); i++)
      {
        struct stat stbuf;
        if (!glnx_fstatat_allow_noent (src_rootfs_fd, toplevel_links[i], &stbuf,
                                       AT_SYMLINK_NOFOLLOW, error))
          return FALSE;
        if (errno == ENOENT)
          continue;

        if (!glnx_renameat (src_rootfs_fd, toplevel_links[i],
                            target_root_dfd, toplevel_links[i], error))
          return FALSE;
      }
  }

  g_print ("Adding rpm-ostree-0-integration.conf\n");
  /* This is useful if we're running in an uninstalled configuration, e.g.
   * during tests. */
  const char *pkglibdir_path
    = g_getenv("RPMOSTREE_UNINSTALLED_PKGLIBDIR") ?: PKGLIBDIR;
  glnx_fd_close int pkglibdir_dfd = -1;

  if (!glnx_opendirat (AT_FDCWD, pkglibdir_path, TRUE, &pkglibdir_dfd, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (target_root_dfd, "usr/lib/tmpfiles.d", 0755, cancellable, error))
    return FALSE;

  if (!glnx_file_copy_at (pkglibdir_dfd, "rpm-ostree-0-integration.conf", NULL,
                          target_root_dfd, "usr/lib/tmpfiles.d/rpm-ostree-0-integration.conf",
                          GLNX_FILE_COPY_NOXATTRS, /* Don't take selinux label */
                          cancellable, error))
    return FALSE;

  /* Handle kernel/initramfs if we're not doing a container */
  if (!container)
    {
      g_print ("Preparing kernel\n");

      /* OSTree needs to own this */
      if (!glnx_shutil_rm_rf_at (src_rootfs_fd, "boot/loader", cancellable, error))
        return FALSE;

      /* The kernel may be in the source rootfs /boot; to handle that, we always
       * rename the source /boot to the target, and will handle everything after
       * that in the target root.
       */
      if (!rename_if_exists (src_rootfs_fd, "boot", target_root_dfd, "boot", error))
        return FALSE;

      if (!process_kernel_and_initramfs (target_root_dfd, treefile,
                                         cancellable, error))
        return glnx_prefix_error (error, "During kernel processing");
    }

  return TRUE;
}

static gboolean
handle_remove_files_from_package (int               rootfs_fd,
                                  RpmOstreeRefSack *refsack,
                                  JsonArray        *removespec,
                                  GCancellable     *cancellable,
                                  GError          **error)
{
  const char *pkgname = json_array_get_string_element (removespec, 0);
  const guint len = json_array_get_length (removespec);
  hy_autoquery HyQuery query = hy_query_create (refsack->sack);
  hy_query_filter (query, HY_PKG_NAME, HY_EQ, pkgname);
  g_autoptr(GPtrArray) pkglist = hy_query_run (query);
  guint npackages = pkglist->len;
  if (npackages == 0)
    return glnx_throw (error, "Unable to find package '%s' specified in remove-from-packages", pkgname);

  for (guint j = 0; j < npackages; j++)
    {
      DnfPackage *pkg = pkglist->pdata[j];
      g_auto(GStrv) pkg_files = dnf_package_get_files (pkg);

      for (guint i = 1; i < len; i++)
        {
          const char *remove_regex_pattern = json_array_get_string_element (removespec, i);

          GRegex *regex = g_regex_new (remove_regex_pattern, G_REGEX_JAVASCRIPT_COMPAT, 0, error);
          if (!regex)
            return FALSE;

          for (char **strviter = pkg_files; strviter && strviter[0]; strviter++)
            {
              const char *file = *strviter;

              if (g_regex_match (regex, file, 0, NULL))
                {
                  if (file[0] == '/')
                    file++;

                  g_print ("Deleting: %s\n", file);
                  if (!glnx_shutil_rm_rf_at (rootfs_fd, file, cancellable, error))
                    return FALSE;
                }
            }
        }
    }

  return TRUE;
}

gboolean
rpmostree_rootfs_symlink_emptydir_at (int rootfs_fd,
                                      const char *dest,
                                      const char *src,
                                      GError **error)
{
  const char *parent = dirname (strdupa (src));
  struct stat stbuf;
  gboolean make_symlink = TRUE;

  /* For maximum compatibility, create parent directories too.  This
   * is necessary when we're doing layering on top of a base commit,
   * and the /var will be empty.  We should probably consider running
   * systemd-tmpfiles to setup the temporary /var.
   */
  if (parent && strcmp (parent, ".") != 0)
    {
      if (!glnx_shutil_mkdir_p_at (rootfs_fd, parent, 0755, NULL, error))
        return FALSE;
    }

  if (!glnx_fstatat_allow_noent (rootfs_fd, src, &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == 0)
    {
      if (S_ISLNK (stbuf.st_mode))
        make_symlink = FALSE;
      else if (S_ISDIR (stbuf.st_mode))
        {
          if (!glnx_unlinkat (rootfs_fd, src, AT_REMOVEDIR, error))
            return FALSE;
        }
    }

  if (make_symlink)
    {
      if (symlinkat (dest, rootfs_fd, src) < 0)
        return glnx_throw_errno_prefix (error, "Symlinking %s", src);
    }
  return TRUE;
}

/**
 * rpmostree_rootfs_prepare_links:
 *
 * Walk over the root filesystem and perform some core conversions
 * from RPM conventions to OSTree conventions.  For example:
 *
 *  - Symlink /usr/local -> /var/usrlocal
 *  - Symlink /var/lib/alternatives -> /usr/lib/alternatives
 *  - Symlink /var/lib/vagrant -> /usr/lib/vagrant
 */
gboolean
rpmostree_rootfs_prepare_links (int           rootfs_fd,
                                GCancellable *cancellable,
                                GError       **error)
{
  if (!glnx_shutil_rm_rf_at (rootfs_fd, "usr/local", cancellable, error))
    return FALSE;
  if (!rpmostree_rootfs_symlink_emptydir_at (rootfs_fd, "../var/usrlocal", "usr/local", error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (rootfs_fd, "usr/lib/alternatives", 0755, cancellable, error))
    return FALSE;
  if (!rpmostree_rootfs_symlink_emptydir_at (rootfs_fd, "../../usr/lib/alternatives", "var/lib/alternatives", error))
    return FALSE;
  if (!glnx_shutil_mkdir_p_at (rootfs_fd, "usr/lib/vagrant", 0755, cancellable, error))
    return FALSE;
  if (!rpmostree_rootfs_symlink_emptydir_at (rootfs_fd, "../../usr/lib/vagrant", "var/lib/vagrant", error))
    return FALSE;

  return TRUE;
}

static gboolean
cleanup_leftover_files (int            rootfs_fd,
                        const char    *subpath,
                        const char    *files[],
                        const char    *prefixes[],
                        GCancellable  *cancellable,
                        GError       **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (rootfs_fd, subpath, TRUE, &dfd_iter, error))
    return glnx_prefix_error (error, "Opening %s", subpath);

  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;
      if (dent->d_type != DT_REG)
        continue;

      const char *name = dent->d_name;

      const gboolean in_files = (files != NULL && g_strv_contains (files, name));
      const gboolean has_prefix =
        (prefixes != NULL && rpmostree_str_has_prefix_in_strv (name, (char**)prefixes, -1));

      if (!in_files && !has_prefix)
        continue;

      if (!glnx_unlinkat (dfd_iter.fd, name, 0, error))
        return FALSE;
    }

  return TRUE;
}

static const char *selinux_leftover_files[] = { "semanage.trans.LOCK",
                                                "semanage.read.LOCK", NULL };
static const char *rpmdb_leftover_files[] = { ".dbenv.lock",
                                              ".rpm.lock", NULL };
static const char *rpmdb_leftover_prefixes[] = { "__db.", NULL };

static gboolean
cleanup_selinux_lockfiles (int            rootfs_fd,
                           GCancellable  *cancellable,
                           GError       **error)
{
  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (rootfs_fd, "usr/etc/selinux", &stbuf, 0, error))
    return FALSE;

  if (errno == ENOENT)
    return TRUE; /* Note early return */

  /* really we only have to do this for the active policy, but let's scan all the dirs */
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (rootfs_fd, "usr/etc/selinux", FALSE, &dfd_iter, error))
    return glnx_prefix_error (error, "Opening /usr/etc/selinux");

  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;
      if (dent->d_type != DT_DIR)
        continue;

      const char *name = dent->d_name;
      if (!cleanup_leftover_files (dfd_iter.fd, name, selinux_leftover_files, NULL,
                                   cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * rpmostree_rootfs_postprocess_common:
 *
 * Walk over the root filesystem and perform some core conversions
 * from RPM conventions to OSTree conventions.  For example:
 *
 *  - Move /etc to /usr/etc
 *  - Clean up RPM db leftovers
 *  - Clean /usr/etc/passwd- backup files and such
 */
gboolean
rpmostree_rootfs_postprocess_common (int           rootfs_fd,
                                     GCancellable *cancellable,
                                     GError       **error)
{
  if (!rename_if_exists (rootfs_fd, "etc", rootfs_fd, "usr/etc", error))
    return FALSE;

  if (!cleanup_leftover_files (rootfs_fd, "usr/share/rpm", rpmdb_leftover_files,
                               rpmdb_leftover_prefixes, cancellable, error))
    return FALSE;

  if (!cleanup_selinux_lockfiles (rootfs_fd, cancellable, error))
    return FALSE;

  if (!rpmostree_passwd_cleanup (rootfs_fd, cancellable, error))
    return FALSE;

  return TRUE;
}

/* Copy external files, if specified in the configuration file, from
 * the context directory to the rootfs.
 */
static gboolean
copy_additional_files (int            rootfs_dfd,
                       GFile         *context_directory,
                       JsonObject    *treefile,
                       GCancellable  *cancellable,
                       GError       **error)
{
  guint len;
  JsonArray *add = NULL;
  if (json_object_has_member (treefile, "add-files"))
    {
      add = json_object_get_array_member (treefile, "add-files");
      len = json_array_get_length (add);
    }
  else
    return TRUE; /* Early return */

  glnx_fd_close int context_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (context_directory), TRUE,
                       &context_dfd, error))
    return FALSE;

  /* Reusable dirname buffer */
  g_autoptr(GString) dnbuf = g_string_new ("");
  for (guint i = 0; i < len; i++)
    {
      const char *src, *dest;
      JsonArray *add_el = json_array_get_array_element (add, i);

      if (!add_el)
        return glnx_throw (error, "Element in add-files is not an array");

      src = _rpmostree_jsonutil_array_require_string_element (add_el, 0, error);
      if (!src)
        return FALSE;

      dest = _rpmostree_jsonutil_array_require_string_element (add_el, 1, error);
      if (!dest)
        return FALSE;
      dest += strspn (dest, "/");
      if (!*dest)
        return glnx_throw (error, "Invalid destination in add-files");
      /* At this point on the filesystem level, the /etc content is already in
       * /usr/etc. But let's be nice and allow people to use add-files into /etc
       * and have it appear in /usr/etc; in most cases we want /usr/etc to just
       * be a libostree implementation detail.
       */
      g_autofree char *dest_owned = NULL;
      if (g_str_has_prefix (dest, "etc/"))
        {
          dest_owned = g_strconcat ("usr/", dest, NULL);
          dest = dest_owned;
        }

      g_print ("Adding file '%s'\n", dest);

      g_string_truncate (dnbuf, 0);
      g_string_append (dnbuf, dest);
      const char *dn = dirname (dnbuf->str);
      g_assert_cmpint (*dn, !=, '/');

      if (!glnx_shutil_mkdir_p_at (rootfs_dfd, dn, 0755, cancellable, error))
        return FALSE;

      /* FIXME: Should probably use GLNX_FILE_COPY_NOXATTRS, but someone
       * may be relying on current semantics?
       */
      if (!glnx_file_copy_at (context_dfd, src, NULL, rootfs_dfd, dest, 0,
                              cancellable, error))
          return glnx_prefix_error (error, "Copying file '%s' into target", src);
    }

  return TRUE;
}

static char *
mutate_os_release (const char    *contents,
                   const char    *base_version,
                   const char    *next_version,
                   GError       **error)
{
  g_auto(GStrv) lines = NULL;
  GString *new_contents = g_string_sized_new (strlen (contents));

  lines = g_strsplit (contents, "\n", -1);
  for (char **it = lines; it && *it; it++)
    {
      const char *line = *it;

      if (strlen (line) == 0)
        continue;

      /* NB: we don't mutate VERSION_ID because some libraries expect well-known
       * values there */
      if (g_str_has_prefix (line, "VERSION=") || \
          g_str_has_prefix (line, "PRETTY_NAME="))
        {
          g_autofree char *new_line = NULL;
          const char *equal = strchr (line, '=');

          g_string_append_len (new_contents, line, equal - line + 1);

          new_line = rpmostree_str_replace (equal + 1, base_version,
                                            next_version, error);
          if (new_line == NULL)
              return NULL;

          g_string_append_printf (new_contents, "%s\n", new_line);
          continue;
        }

      g_string_append_printf (new_contents, "%s\n", line);
    }

  /* add a bona fide ostree entry */
  g_string_append_printf (new_contents, "OSTREE_VERSION=%s\n", next_version);

  return g_string_free (new_contents, FALSE);
}

/* Move etc -> usr/etc in the rootfs, and run through treefile
 * postprocessing.
 */
gboolean
rpmostree_treefile_postprocessing (int            rootfs_fd,
                                   GFile         *context_directory,
                                   GBytes        *serialized_treefile,
                                   JsonObject    *treefile,
                                   const char    *next_version,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  if (!rename_if_exists (rootfs_fd, "etc", rootfs_fd, "usr/etc", error))
    return FALSE;

  JsonArray *units = NULL;
  if (json_object_has_member (treefile, "units"))
    units = json_object_get_array_member (treefile, "units");

  guint len;
  if (units)
    len = json_array_get_length (units);
  else
    len = 0;

  {
    glnx_fd_close int multiuser_wants_dfd = -1;

    if (!glnx_shutil_mkdir_p_at (rootfs_fd, "usr/etc/systemd/system/multi-user.target.wants", 0755,
                                 cancellable, error))
      return FALSE;
    if (!glnx_opendirat (rootfs_fd, "usr/etc/systemd/system/multi-user.target.wants", TRUE,
                         &multiuser_wants_dfd, error))
      return FALSE;

    for (guint i = 0; i < len; i++)
      {
        const char *unitname = _rpmostree_jsonutil_array_require_string_element (units, i, error);
        g_autofree char *symlink_target = NULL;
        struct stat stbuf;

        if (!unitname)
          return FALSE;

        symlink_target = g_strconcat ("/usr/lib/systemd/system/", unitname, NULL);

        if (fstatat (multiuser_wants_dfd, unitname, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
          {
            if (errno != ENOENT)
              return glnx_throw_errno_prefix (error, "fstatat(%s)", unitname);
          }
        else
          continue;

        g_print ("Adding %s to multi-user.target.wants\n", unitname);

        if (symlinkat (symlink_target, multiuser_wants_dfd, unitname) < 0)
          return glnx_throw_errno_prefix (error, "symlinkat(%s)", unitname);
      }
  }

  if (!glnx_shutil_mkdir_p_at (rootfs_fd, "usr/share/rpm-ostree", 0755, cancellable, error))
    return FALSE;

  { gsize len;
    const guint8 *buf = g_bytes_get_data (serialized_treefile, &len);

    if (!glnx_file_replace_contents_at (rootfs_fd, "usr/share/rpm-ostree/treefile.json",
                                        buf, len, GLNX_FILE_REPLACE_NODATASYNC,
                                        cancellable, error))
      return FALSE;
  }

  const char *default_target = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile, "default_target",
                                                              &default_target, error))
    return FALSE;

  if (default_target != NULL)
    {
      g_autofree char *dest_default_target_path =
        g_strconcat ("/usr/lib/systemd/system/", default_target, NULL);

      static const char default_target_path[] = "usr/etc/systemd/system/default.target";
      (void) unlinkat (rootfs_fd, default_target_path, 0);

      if (symlinkat (dest_default_target_path, rootfs_fd, default_target_path) < 0)
        return glnx_throw_errno_prefix (error, "symlinkat(%s)", default_target_path);
    }

  JsonArray *remove = NULL;
  if (json_object_has_member (treefile, "remove-files"))
    {
      remove = json_object_get_array_member (treefile, "remove-files");
      len = json_array_get_length (remove);
    }
  else
    len = 0;

  /* Put /etc back for backwards compatibility */
  if (len > 0)
    {
      if (!rename_if_exists (rootfs_fd, "usr/etc", rootfs_fd, "etc", error))
        return FALSE;
    }
  /* Process the remove-files element */
  for (guint i = 0; i < len; i++)
    {
      const char *val = _rpmostree_jsonutil_array_require_string_element (remove, i, error);

      if (!val)
        return FALSE;
      if (g_path_is_absolute (val))
        return glnx_throw (error, "'remove' elements must be relative");
      g_assert_cmpint (val[0], !=, '/');
      g_assert (strstr (val, "..") == NULL);

      g_print ("Deleting: %s\n", val);
      if (!glnx_shutil_rm_rf_at (rootfs_fd, val, cancellable, error))
        return FALSE;
    }
  if (len > 0)
    {
      /* And put /etc back to /usr/etc */
      if (!rename_if_exists (rootfs_fd, "etc", rootfs_fd, "usr/etc", error))
        return FALSE;
    }

  /* This works around a potential issue with libsolv if we go down the
   * rpmostree_get_pkglist_for_root() path. Though rpm has been using the
   * /usr/share/rpm location (since the RpmOstreeContext set the _dbpath macro),
   * the /var/lib/rpm directory will still exist, but be empty. libsolv gets
   * confused because it sees the /var/lib/rpm dir and doesn't even try the
   * /usr/share/rpm location, and eventually dies when it tries to load the
   * data. XXX: should probably send a patch upstream to libsolv.
   *
   * So we set the symlink now. This is also what we do on boot anyway for
   * compatibility reasons using tmpfiles.
   * */
  if (!glnx_shutil_rm_rf_at (rootfs_fd, "var/lib/rpm", cancellable, error))
    return FALSE;
  if (symlinkat ("../../usr/share/rpm", rootfs_fd, "var/lib/rpm") < 0)
    return glnx_throw_errno_prefix (error, "symlinkat(%s)", "var/lib/rpm");

  if (json_object_has_member (treefile, "remove-from-packages"))
    {
      g_autoptr(RpmOstreeRefSack) refsack = NULL;

      remove = json_object_get_array_member (treefile, "remove-from-packages");
      len = json_array_get_length (remove);

      if (!rpmostree_get_pkglist_for_root (rootfs_fd, ".", &refsack, NULL,
                                           cancellable, error))
        return glnx_prefix_error (error, "Reading package set");

      /* Backwards compatibility */
      if (!rename_if_exists (rootfs_fd, "usr/etc", rootfs_fd, "etc", error))
        return FALSE;

      for (guint i = 0; i < len; i++)
        {
          JsonArray *elt = json_array_get_array_element (remove, i);
          if (!handle_remove_files_from_package (rootfs_fd, refsack, elt, cancellable, error))
            return FALSE;
        }

      /* Backwards compatibility */
      if (!rename_if_exists (rootfs_fd, "etc", rootfs_fd, "usr/etc", error))
        return FALSE;
    }

  {
    const char *base_version = NULL;

    if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile,
                                                                "mutate-os-release",
                                                                &base_version,
                                                                error))
      return FALSE;

    if (base_version != NULL && next_version == NULL)
      {
        g_print ("Ignoring mutate-os-release: no commit version specified.");
      }
    else if (base_version != NULL)
      {
        /* let's try to find the first non-symlink */
        const char *os_release[] = {
          "usr/etc/os-release",
          "usr/lib/os-release",
          "usr/lib/os.release.d/os-release-fedora"
        };

        /* fallback on just overwriting etc/os-release */
        const char *path = os_release[0];

        for (guint i = 0; i < G_N_ELEMENTS (os_release); i++)
          {
            struct stat stbuf;

            if (!glnx_fstatat (rootfs_fd, os_release[i], &stbuf,
                               AT_SYMLINK_NOFOLLOW, error))
              return FALSE;

            if (S_ISREG (stbuf.st_mode))
              {
                path = os_release[i];
                break;
              }
          }

        g_print ("Mutating /%s\n", path);

        g_autofree char *contents = glnx_file_get_contents_utf8_at (rootfs_fd, path, NULL,
                                                                    cancellable, error);
        if (contents == NULL)
          return FALSE;

        g_autofree char *new_contents = mutate_os_release (contents, base_version,
                                                           next_version, error);
        if (new_contents == NULL)
          return FALSE;

        if (!glnx_file_replace_contents_at (rootfs_fd, path,
                                            (guint8*)new_contents, -1, 0,
                                            cancellable, error))
          return FALSE;
      }
  }

  /* Copy in additional files before postprocessing */
  if (!copy_additional_files (rootfs_fd, context_directory, treefile, cancellable, error))
    return FALSE;

  const char *postprocess_script = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile, "postprocess-script",
                                                              &postprocess_script, error))
    return FALSE;

  if (postprocess_script)
    {
      const char *bn = glnx_basename (postprocess_script);
      g_autofree char *src = NULL;
      g_autofree char *binpath = NULL;

      if (g_path_is_absolute (postprocess_script))
        src = g_strdup (postprocess_script);
      else
        src = g_build_filename (gs_file_get_path_cached (context_directory), postprocess_script, NULL);

      binpath = g_strconcat ("/usr/bin/rpmostree-postprocess-", bn, NULL);
      /* Clone all the things */

      /* Note we need to make binpath *not* absolute here */
      const char *target_binpath = binpath + 1;
      g_assert_cmpint (*target_binpath, !=, '/');
      if (!glnx_file_copy_at (AT_FDCWD, src, NULL, rootfs_fd, target_binpath,
                              GLNX_FILE_COPY_NOXATTRS, cancellable, error))
        return FALSE;

      g_print ("Executing postprocessing script '%s'\n", bn);

      {
        char *child_argv[] = { binpath, NULL };
        if (!run_bwrap_mutably (rootfs_fd, binpath, child_argv, cancellable, error))
          return glnx_prefix_error (error, "While executing postprocessing script '%s'", bn);
      }

      if (!glnx_unlinkat (rootfs_fd, target_binpath, 0, error))
        return FALSE;

      g_print ("Finished postprocessing script '%s'\n", bn);
    }

  return TRUE;
}

/**
 * rpmostree_prepare_rootfs_for_commit:
 *
 * Walk over the root filesystem and perform some core conversions
 * from RPM conventions to OSTree conventions.  For example:
 *
 *  * Checksum the kernel in /boot
 *  * Migrate content in /var to systemd-tmpfiles
 */
gboolean
rpmostree_prepare_rootfs_for_commit (int            workdir_dfd,
                                     int           *inout_rootfs_fd,
                                     const char    *rootfs_name,
                                     JsonObject    *treefile,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
  const char *temp_new_root = "tmp-new-rootfs";
  glnx_fd_close int target_root_dfd = -1;

  if (!glnx_ensure_dir (workdir_dfd, temp_new_root, 0755, error))
    return FALSE;

  if (!glnx_opendirat (workdir_dfd, temp_new_root, TRUE,
                       &target_root_dfd, error))
    return FALSE;

  if (!create_rootfs_from_pkgroot_content (target_root_dfd, *inout_rootfs_fd, treefile,
                                           cancellable, error))
    return glnx_prefix_error (error, "Finalizing rootfs");

  (void) close (*inout_rootfs_fd);

  if (!glnx_shutil_rm_rf_at (workdir_dfd, rootfs_name, cancellable, error))
    return FALSE;

  if (!glnx_renameat (workdir_dfd, temp_new_root,
                      workdir_dfd, rootfs_name, error))
    return FALSE;

  *inout_rootfs_fd = target_root_dfd;
  target_root_dfd = -1;  /* Transfer ownership */

  return TRUE;
}

struct CommitThreadData {
  volatile gint done;
  off_t n_bytes;
  off_t n_processed;
  volatile gint percent;
  OstreeRepo *repo;
  int rootfs_fd;
  OstreeMutableTree *mtree;
  OstreeSePolicy *sepolicy;
  OstreeRepoCommitModifier *commit_modifier;
  gboolean success;
  GCancellable *cancellable;
  GError **error;
};

static GVariant *
read_xattrs_cb (OstreeRepo     *repo,
                const char     *relpath,
                GFileInfo      *file_info,
                gpointer        user_data)
{
  struct CommitThreadData *tdata = user_data;
  int rootfs_fd = tdata->rootfs_fd;
  /* If you have a use case for something else, file an issue */
  static const char *accepted_xattrs[] =
    { "security.capability", /* https://lwn.net/Articles/211883/ */
      "user.pax.flags" /* https://github.com/projectatomic/rpm-ostree/issues/412 */
    };
  guint i;
  g_autoptr(GVariant) existing_xattrs = NULL;
  g_autoptr(GVariantIter) viter = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  GVariant *key, *value;
  GVariantBuilder builder;

  if (relpath[0] == '/')
    relpath++;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));

  if (!*relpath)
    {
      if (!glnx_fd_get_all_xattrs (rootfs_fd, &existing_xattrs, NULL, error))
        goto out;
    }
  else
    {
      if (!glnx_dfd_name_get_all_xattrs (rootfs_fd, relpath, &existing_xattrs,
                                         NULL, error))
        goto out;
    }

  if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
    {
      tdata->n_processed += g_file_info_get_size (file_info);
      g_atomic_int_set (&tdata->percent, (gint)((100.0*tdata->n_processed)/tdata->n_bytes));
    }

  viter = g_variant_iter_new (existing_xattrs);

  while (g_variant_iter_loop (viter, "(@ay@ay)", &key, &value))
    {
      for (i = 0; i < G_N_ELEMENTS (accepted_xattrs); i++)
        {
          const char *validkey = accepted_xattrs[i];
          const char *attrkey = g_variant_get_bytestring (key);
          if (g_str_equal (validkey, attrkey))
            g_variant_builder_add (&builder, "(@ay@ay)", key, value);
        }
    }

 out:
  if (local_error)
    {
      g_variant_builder_clear (&builder);
      /* Unfortunately we have no way to throw from this callback */
      g_printerr ("Failed to read xattrs of '%s': %s\n",
                  relpath, local_error->message);
      exit (1);
    }
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static gboolean
count_filesizes (int dfd,
                 const char *path,
                 off_t *out_n_bytes,
                 GCancellable *cancellable,
                 GError **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  
  if (!glnx_dirfd_iterator_init_at (dfd, path, TRUE, &dfd_iter, error))
    return FALSE;
  
  while (TRUE)
    {
      struct dirent *dent = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      if (dent->d_type == DT_DIR)
        {
          if (!count_filesizes (dfd_iter.fd, dent->d_name, out_n_bytes,
                                cancellable, error))
            return FALSE;
        }
      else
        {
          struct stat stbuf;

          if (!glnx_fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
            return FALSE;

          (*out_n_bytes) += stbuf.st_size;
        }
    }

  return TRUE;
}

static gpointer
write_dfd_thread (gpointer datap)
{
  struct CommitThreadData *data = datap;

  if (!ostree_repo_write_dfd_to_mtree (data->repo, data->rootfs_fd, ".",
                                       data->mtree,
                                       data->commit_modifier,
                                       data->cancellable, data->error))
    goto out;

  data->success = TRUE;
 out:
  g_atomic_int_inc (&data->done);
  g_main_context_wakeup (NULL);
  return NULL;
}

static gboolean
on_progress_timeout (gpointer datap)
{
  struct CommitThreadData *data = datap;
  const gint percent = g_atomic_int_get (&data->percent);

  glnx_console_progress_text_percent ("Committing:", percent);

  return TRUE;
}

gboolean
rpmostree_commit (int            rootfs_fd,
                  OstreeRepo    *repo,
                  const char    *refname,
                  const char    *write_commitid_to,
                  GVariant      *metadata,
                  const char    *gpg_keyid,
                  gboolean       enable_selinux,
                  OstreeRepoDevInoCache *devino_cache,
                  char         **out_new_revision,
                  GCancellable  *cancellable,
                  GError       **error)
{
  /* hardcode targeted policy for now */
  g_autoptr(OstreeSePolicy) sepolicy = NULL;
  if (enable_selinux)
    {
      if (!rpmostree_prepare_rootfs_get_sepolicy (rootfs_fd, &sepolicy, cancellable, error))
        return FALSE;
    }

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;

  g_autoptr(OstreeMutableTree) mtree = ostree_mutable_tree_new ();
  /* We may make this configurable if someone complains about including some
   * unlabeled content, but I think the fix for that is to ensure that policy is
   * labeling it.
   */
  OstreeRepoCommitModifierFlags modifier_flags = OSTREE_REPO_COMMIT_MODIFIER_FLAGS_ERROR_ON_UNLABELED;
  /* If changing this, also look at changing rpmostree-unpacker.c */
  g_autoptr(OstreeRepoCommitModifier) commit_modifier =
    ostree_repo_commit_modifier_new (modifier_flags, NULL, NULL, NULL);
  struct CommitThreadData tdata = { 0, };
  ostree_repo_commit_modifier_set_xattr_callback (commit_modifier,
                                                  read_xattrs_cb, NULL,
                                                  &tdata);

  if (sepolicy && ostree_sepolicy_get_name (sepolicy) != NULL)
    ostree_repo_commit_modifier_set_sepolicy (commit_modifier, sepolicy);
  else if (enable_selinux)
    return glnx_throw (error, "SELinux enabled, but no policy found");

  if (devino_cache)
    ostree_repo_commit_modifier_set_devino_cache (commit_modifier, devino_cache);

  off_t n_bytes = 0;
  if (!count_filesizes (rootfs_fd, ".", &n_bytes, cancellable, error))
    return FALSE;

  tdata.n_bytes = n_bytes;
  tdata.repo = repo;
  tdata.rootfs_fd = rootfs_fd;
  tdata.mtree = mtree;
  tdata.sepolicy = sepolicy;
  tdata.commit_modifier = commit_modifier;
  tdata.error = error;

  g_autoptr(GThread) commit_thread = NULL;
  g_auto(GLnxConsoleRef) console = { 0, };
  g_autoptr(GSource) progress_src = NULL;

  glnx_console_lock (&console);

  commit_thread = g_thread_new ("commit", write_dfd_thread, &tdata);

  progress_src = g_timeout_source_new_seconds (console.is_tty ? 1 : 5);
  g_source_set_callback (progress_src, on_progress_timeout, &tdata, NULL);
  g_source_attach (progress_src, NULL);

  while (g_atomic_int_get (&tdata.done) == 0)
    g_main_context_iteration (NULL, TRUE);

  glnx_console_progress_text_percent ("Committing:", 100.0);
  glnx_console_unlock (&console);

  g_thread_join (g_steal_pointer (&commit_thread));
  if (!tdata.success)
    return glnx_prefix_error (error, "While writing rootfs to mtree");

  g_autoptr(GFile) root_tree = NULL;
  if (!ostree_repo_write_mtree (repo, mtree, &root_tree, cancellable, error))
    return glnx_prefix_error (error, "While writing tree");

  g_autofree char *parent_revision = NULL;
  if (refname)
    {
      if (!ostree_repo_resolve_rev (repo, refname, TRUE, &parent_revision, error))
        return FALSE;
    }

  g_autofree char *new_revision = NULL;
  if (!ostree_repo_write_commit (repo, parent_revision, "", "", metadata,
                                 (OstreeRepoFile*)root_tree, &new_revision,
                                 cancellable, error))
    return glnx_prefix_error (error, "While writing commit");

  if (gpg_keyid)
    {
      if (!ostree_repo_sign_commit (repo, new_revision, gpg_keyid, NULL,
                                    cancellable, error))
        return glnx_prefix_error (error, "While signing commit");
    }

  if (write_commitid_to)
    {
      if (!g_file_set_contents (write_commitid_to, new_revision, -1, error))
        return glnx_prefix_error (error, "While writing to '%s'", write_commitid_to);
    }
  else if (refname)
    ostree_repo_transaction_set_ref (repo, NULL, refname, new_revision);

  OstreeRepoTransactionStats stats = { 0, };
  if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
    return glnx_prefix_error (error, "Commit");

  g_print ("Metadata Total: %u\n", stats.metadata_objects_total);
  g_print ("Metadata Written: %u\n", stats.metadata_objects_written);
  g_print ("Content Total: %u\n", stats.content_objects_total);
  g_print ("Content Written: %u\n", stats.content_objects_written);
  g_print ("Content Bytes Written: %" G_GUINT64_FORMAT "\n", stats.content_bytes_written);
  if (out_new_revision)
    *out_new_revision = g_steal_pointer (&new_revision);
  return TRUE;
}
