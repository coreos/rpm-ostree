/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014,2017 Colin Walters <walters@verbum.org>
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
#include <stdio.h>
#include <utime.h>
#include <err.h>
#include <sys/types.h>
#include <unistd.h>
#include <libglnx.h>
#include <stdlib.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include "rpmostree-passwd-util.h"
#include "rpmostree-core.h"
#include "rpmostree-kernel.h"
#include "rpmostree-bwrap.h"
#include "rpmostree-rust.h"
#include "rpmostree-util.h"

static const char usrlib_ostreeboot[] = "usr/lib/ostree-boot";

/* Keep this in sync with ostree/src/libostree/ostree-sysroot-deploy.c:get_kernel_from_tree().
 * Note they are of necessity slightly different since rpm-ostree needs
 * to support grabbing wherever the Fedora kernel RPM dropped files as well.
 */
static gboolean
find_kernel_and_initramfs_in_bootdir (int          rootfs_dfd,
                                      const char  *bootdir,
                                      char       **out_kernel,
                                      char       **out_initramfs,
                                      GCancellable *cancellable,
                                      GError     **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  glnx_autofd int dfd = -1;
  g_autofree char* ret_kernel = NULL;
  g_autofree char* ret_initramfs = NULL;

  *out_kernel = *out_initramfs = NULL;

  dfd = glnx_opendirat_with_errno (rootfs_dfd, bootdir, FALSE);
  if (dfd < 0)
    {
      if (errno == ENOENT)
        return TRUE; /* Note early return */
      else
        return glnx_throw_errno_prefix (error, "opendir(%s)", bootdir);
    }
  if (!glnx_dirfd_iterator_init_take_fd (&dfd, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      const char *name;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      if (dent->d_type != DT_REG)
        continue;

      name = dent->d_name;

      /* Current Fedora 23 kernel.spec installs as just vmlinuz */
      if (g_str_equal (name, "vmlinuz") || g_str_has_prefix (name, "vmlinuz-"))
        {
          if (ret_kernel)
            return glnx_throw (error, "Multiple vmlinuz- in %s", bootdir);
          ret_kernel = g_strconcat (bootdir, "/", name, NULL);
        }
      else if (g_str_equal (name, "initramfs.img") || g_str_has_prefix (name, "initramfs-"))
        {
          if (ret_initramfs)
            return glnx_throw (error, "Multiple initramfs- in %s", bootdir);
          ret_initramfs = g_strconcat (bootdir, "/", name, NULL);
        }
    }

  *out_kernel = g_steal_pointer (&ret_kernel);
  *out_initramfs = g_steal_pointer (&ret_initramfs);
  return TRUE;
}

/* Given a directory @subpath, find the first child that is a directory,
 * returning it in @out_subdir.  If there are multiple directories,
 * return an error.
 */
static gboolean
find_ensure_one_subdirectory (int            rootfs_dfd,
                              const char    *subpath,
                              char         **out_subdir,
                              GCancellable  *cancellable,
                              GError       **error)
{
  g_autofree char *ret_subdir = NULL;
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  if (!glnx_dirfd_iterator_init_at (rootfs_dfd, subpath, TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      if (dent->d_type != DT_DIR)
        continue;

      if (ret_subdir)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Multiple subdirectories found in: %s", subpath);
          return FALSE;
        }
      ret_subdir = g_strconcat (subpath, "/", dent->d_name, NULL);
    }

  *out_subdir = g_steal_pointer (&ret_subdir);
  return TRUE;
}

static gboolean
kernel_remove_in (int rootfs_dfd,
                  const char *bootdir,
                  GCancellable *cancellable,
                  GError **error)
{
  g_autofree char* kernel_path = NULL;
  g_autofree char* initramfs_path = NULL;
  if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir,
                                             &kernel_path, &initramfs_path,
                                             cancellable, error))
    return FALSE;

  if (kernel_path)
    {
      if (!glnx_unlinkat (rootfs_dfd, kernel_path, 0, error))
        return FALSE;
    }
  if (initramfs_path)
    {
      if (!glnx_unlinkat (rootfs_dfd, initramfs_path, 0, error))
        return FALSE;
    }

  return TRUE;

}

/* Given a root filesystem, delete all kernel/initramfs data from it.
 * The rpm filelist for the kernel isn't aware of all the places we
 * copy the data, such as `/usr/lib/ostree-boot`.
 * Used by `rpm-ostree override replace ./kernel-42.x86_64.rpm`.
 */
gboolean
rpmostree_kernel_remove (int rootfs_dfd,
                         GCancellable *cancellable,
                         GError **error)
{
  g_autofree char* modversion_dir = NULL;
  if (!find_ensure_one_subdirectory (rootfs_dfd, "usr/lib/modules",
                                     &modversion_dir, cancellable, error))
    return FALSE;
  if (modversion_dir)
    {
      if (!kernel_remove_in (rootfs_dfd, modversion_dir, cancellable, error))
        return FALSE;
      glnx_autofd int modversion_dfd = -1;
      if (!glnx_opendirat (rootfs_dfd, modversion_dir, TRUE, &modversion_dfd, error))
        return FALSE;
      /* See `/usr/lib/kernel/install.d/50-depmod.install`
       * which is run by `kernel-install remove` from RPM `%postun`.
       *
       * TODO: Add a depmod --clean <kver> command.
       */
      const char *depmod_files[] = {"modules.alias", "modules.alias.bin", "modules.builtin.bin",
                                    "modules.dep", "modules.dep.bin", "modules.devname",
                                    "modules.softdep", "modules.symbols", "modules.symbols.bin" };
      for (guint i = 0; i < G_N_ELEMENTS (depmod_files); i++)
        {
          const char *name = depmod_files[i];
          if (unlinkat (modversion_dfd, name, 0) < 0)
            {
              if (errno != ENOENT)
                return glnx_throw_errno_prefix (error, "unlinkat(%s)", name);
            }
        }
    }
  if (!kernel_remove_in (rootfs_dfd, "usr/lib/ostree-boot", cancellable, error))
    return FALSE;
  if (!kernel_remove_in (rootfs_dfd, "boot", cancellable, error))
    return FALSE;

  return TRUE;
}

/* Given a root filesystem, return a GVariant of format (sssms):
 *  - kver: uname -r equivalent
 *  - bootdir: Path to the boot directory
 *  - kernel_path: Relative (to rootfs) path to kernel
 *  - initramfs_path: Relative (to rootfs) path to initramfs (may be NULL if no initramfs)
 */
GVariant *
rpmostree_find_kernel (int rootfs_dfd,
                       GCancellable *cancellable,
                       GError **error)
{
  /* Fetch the kver from /usr/lib/modules */
  g_autofree char* modversion_dir = NULL;
  if (!find_ensure_one_subdirectory (rootfs_dfd, "usr/lib/modules",
                                     &modversion_dir, cancellable, error))
    return NULL;

  if (!modversion_dir)
    return glnx_null_throw (error, "/usr/lib/modules is empty");

  const char *kver = glnx_basename (modversion_dir);

  /* First, look for the kernel in the canonical ostree directory */
  g_autofree char* kernel_path = NULL;
  g_autofree char* initramfs_path = NULL;
  g_autofree char *bootdir = g_strdup (usrlib_ostreeboot);
  if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir,
                                             &kernel_path, &initramfs_path,
                                             cancellable, error))
    return NULL;

  /* Next, the traditional /boot */
  if (kernel_path == NULL)
    {
      g_free (bootdir);
      bootdir = g_strdup ("boot");

      if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir,
                                                 &kernel_path, &initramfs_path,
                                                 cancellable, error))
        return NULL;
    }

  /* Finally, the newer model of having the kernel with the modules */
  if (kernel_path == NULL)
    {
      g_free (bootdir);
      bootdir = g_steal_pointer (&modversion_dir);
      if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir,
                                                 &kernel_path, &initramfs_path,
                                                 cancellable, error))
        return NULL;
    }

  if (kernel_path == NULL)
    return glnx_null_throw (error, "Unable to find kernel (vmlinuz) in /boot "
                                   "or /usr/lib/modules (bootdir=%s)", bootdir);

  return g_variant_ref_sink (g_variant_new ("(sssms)", kver, bootdir, kernel_path, initramfs_path));
}

/* Given a @rootfs_dfd and path to kernel/initramfs that live in
 * usr/lib/modules/$kver, possibly update @bootdir to use them. @bootdir should
 * be one of either /usr/lib/ostree-boot or /boot. If @only_if_found is set, we
 * do the copy only if we find a kernel; this way we avoid e.g. touching /boot
 * if it isn't being used.
 */
static gboolean
copy_kernel_into (int rootfs_dfd,
                  const char *kver,
                  const char *boot_checksum_str,
                  const char *kernel_modules_path,
                  const char *initramfs_modules_path,
                  gboolean only_if_found,
                  const char *bootdir,
                  GCancellable *cancellable,
                  GError **error)
{
  g_autofree char *legacy_kernel_path = NULL;
  g_autofree char* legacy_initramfs_path = NULL;
  if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir,
                                             &legacy_kernel_path, &legacy_initramfs_path,
                                             cancellable, error))
    return FALSE;

  /* No kernel found? Skip to the next if we're in "auto"
   * mode i.e. only update if found.
   */
  if (!legacy_kernel_path && only_if_found)
    return TRUE;

  /* Update kernel */
  if (legacy_kernel_path)
    {
      if (!glnx_unlinkat (rootfs_dfd, legacy_kernel_path, 0, error))
        return FALSE;
      g_free (legacy_kernel_path);
    }
  legacy_kernel_path = g_strconcat (bootdir, "/", "vmlinuz-", kver, "-", boot_checksum_str, NULL);
  if (linkat (rootfs_dfd, kernel_modules_path, rootfs_dfd, legacy_kernel_path, 0) < 0)
    return glnx_throw_errno_prefix (error, "linkat(%s)", legacy_kernel_path);

  /* Update initramfs */
  if (legacy_initramfs_path)
    {
      if (!glnx_unlinkat (rootfs_dfd, legacy_initramfs_path, 0, error))
        return FALSE;
      g_free (legacy_initramfs_path);
    }
  legacy_initramfs_path = g_strconcat (bootdir, "/", "initramfs-", kver, ".img-", boot_checksum_str, NULL);
  if (linkat (rootfs_dfd, initramfs_modules_path, rootfs_dfd, legacy_initramfs_path, 0) < 0)
    return glnx_throw_errno_prefix (error, "linkat(%s)", legacy_initramfs_path);

  return TRUE;
}

/* Given a kernel path and a temporary initramfs, place them in their final
 * location. We handle /usr/lib/modules as well as the /usr/lib/ostree-boot and
 * /boot paths where we need to pre-compute their checksum.
 */
gboolean
rpmostree_finalize_kernel (int rootfs_dfd,
                           const char *bootdir,
                           const char *kver,
                           const char *kernel_path,
                           GLnxTmpfile *initramfs_tmpf,
                           RpmOstreeFinalizeKernelDestination dest,
                           GCancellable *cancellable,
                           GError **error)
{
  const char slash_bootdir[] = "boot";
  g_autofree char *modules_bootdir = g_build_filename ("usr/lib/modules", kver, NULL);

  /* Calculate the sha256sum of the kernel+initramfs (called the "boot
   * checksum"). We checksum the initramfs from the tmpfile fd (via mmap()) to
   * avoid writing it to disk in another temporary location.
   */
  g_autoptr(GChecksum) boot_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (!_rpmostree_util_update_checksum_from_file (boot_checksum, rootfs_dfd, kernel_path,
                                                  cancellable, error))
    return FALSE;

  g_autofree char *initramfs_modules_path =
    g_build_filename (modules_bootdir, "initramfs.img", NULL);

  { g_autoptr(GMappedFile) mfile = g_mapped_file_new_from_fd (initramfs_tmpf->fd, FALSE, error);
    if (!mfile)
      return FALSE;
    g_checksum_update (boot_checksum, (guint8*)g_mapped_file_get_contents (mfile),
                       g_mapped_file_get_length (mfile));

    /* Replace the initramfs */
    if (unlinkat (rootfs_dfd, initramfs_modules_path, 0) < 0)
      {
        if (errno != ENOENT)
          return glnx_throw_errno_prefix (error, "unlinkat(%s)", initramfs_modules_path);
      }
    if (!glnx_link_tmpfile_at (initramfs_tmpf, GLNX_LINK_TMPFILE_NOREPLACE,
                               rootfs_dfd, initramfs_modules_path,
                               error))
      return FALSE;
  }

  const char *boot_checksum_str = g_checksum_get_string (boot_checksum);

  g_autofree char *kernel_modules_path = g_build_filename (modules_bootdir, "vmlinuz", NULL);
  /* It's possible the bootdir is already the modules directory; in that case,
   * we don't need to rename.
   */
  if (!g_str_equal (kernel_path, kernel_modules_path))
    {
      g_assert_cmpstr (bootdir, !=, modules_bootdir);
      /* Ensure that the /usr/lib/modules kernel is the same as the source.
       * Right now we don't support overriding the kernel, but to be
       * conservative let's relink (unlink/link). We don't just rename() because
       * for _AUTO mode we still want to find the kernel in the old path
       * (probably /usr/lib/ostree-boot) and update as appropriate.
       */
      if (unlinkat (rootfs_dfd, kernel_modules_path, 0) < 0)
        {
          if (errno != ENOENT)
            return glnx_throw_errno_prefix (error, "unlinkat(%s)", kernel_modules_path);
        }
      if (linkat (rootfs_dfd, kernel_path, rootfs_dfd, kernel_modules_path, 0) < 0)
        return glnx_throw_errno_prefix (error, "linkat(%s)", kernel_modules_path);
    }

  /* If there's an HMAC file, fix the path to the kernel in it to be relative. Right now,
   * the kernel spec encodes `/boot/vmlinux-$kver`, which of course not going to work for
   * us. We should work towards making this change directly into the kernel spec. */
  g_autofree char *hmac_path = g_build_filename (modules_bootdir, ".vmlinuz.hmac", NULL);
  if (!glnx_fstatat_allow_noent (rootfs_dfd, hmac_path, NULL, 0, error))
    return FALSE;
  if (errno == 0)
    {
      g_autofree char *contents = glnx_file_get_contents_utf8_at (rootfs_dfd, hmac_path,
                                                                  NULL, cancellable, error);
      if (contents == NULL)
        return FALSE;

      /* rather than trying to parse and understand the *sum format, just hackily replace */
      g_autofree char *old_path = g_strconcat ("  /boot/vmlinuz-", kver, NULL);
      g_autofree char *new_path = g_strconcat ("  vmlinuz-", kver, NULL);
      g_autofree char *new_contents =
        rpmostree_str_replace (contents, old_path, new_path, error);
      if (!new_contents)
        return FALSE;

      /* sanity check there are no '/' in there; that way too we just error out if the path
       * or format changes (but really, this should be a temporary hack...) */
      if (strchr (new_contents, '/') != 0)
        return glnx_throw (error, "Unexpected / in .vmlinuz.hmac: %s", new_contents);

      if (!glnx_file_replace_contents_at (rootfs_dfd, hmac_path,
                                          (guint8*)new_contents, -1, 0,
                                          cancellable, error))
        return FALSE;
    }

  /* Update /usr/lib/ostree-boot and /boot (if desired) */
  const gboolean only_if_found = (dest == RPMOSTREE_FINALIZE_KERNEL_AUTO);
  if (only_if_found || dest >= RPMOSTREE_FINALIZE_KERNEL_USRLIB_OSTREEBOOT)
    {
      if (!copy_kernel_into (rootfs_dfd, kver, boot_checksum_str,
                             kernel_modules_path, initramfs_modules_path,
                             only_if_found, usrlib_ostreeboot,
                             cancellable, error))
        return FALSE;
    }
  if (only_if_found || dest >= RPMOSTREE_FINALIZE_KERNEL_SLASH_BOOT)
    {
      if (!copy_kernel_into (rootfs_dfd, kver, boot_checksum_str,
                             kernel_modules_path, initramfs_modules_path,
                             only_if_found, slash_bootdir,
                             cancellable, error))
        return FALSE;
    }
  return TRUE;
}

static void
dracut_child_setup (gpointer data)
{
  int fd = GPOINTER_TO_INT (data);

  /* Move the tempfile fd to 3 (and without the cloexec flag) */
  if (dup2 (fd, 3) < 0)
    err (1, "dup2");
}

gboolean
rpmostree_run_dracut (int     rootfs_dfd,
                      const char *const* argv,
                      const char *kver,
                      const char *rebuild_from_initramfs,
                      gboolean     use_root_etc,
                      GLnxTmpDir  *dracut_host_tmpdir,
                      GLnxTmpfile *out_initramfs_tmpf,
                      GCancellable  *cancellable,
                      GError **error)
{
  gboolean ret = FALSE;
  /* Shell wrapper around dracut to write to the O_TMPFILE fd;
   * at some point in the future we should add --fd X instead of -f
   * to dracut.
   */
  static const char rpmostree_dracut_wrapper_path[] = "usr/bin/rpmostree-dracut-wrapper";
  /* This also hardcodes a few arguments */
  g_autofree char * rpmostree_dracut_wrapper =
    g_strdup_printf (
    "#!/usr/bin/bash\n"
    "set -euo pipefail\n"
    "export PATH=%s:${PATH}\n"
    "extra_argv=; if (dracut --help; true) | grep -q -e --reproducible; then extra_argv=\"--reproducible --gzip\"; fi\n"
    "mkdir -p /tmp/dracut && dracut $extra_argv -v --add ostree --tmpdir=/tmp/dracut -f /tmp/initramfs.img \"$@\"\n"
    "cat /tmp/initramfs.img >/proc/self/fd/3\n",
    ror_cliwrap_destdir ());
  g_autoptr(RpmOstreeBwrap) bwrap = NULL;
  g_autoptr(GPtrArray) rebuild_argv = NULL;
  g_auto(GLnxTmpfile) tmpf = { 0, };
  g_autoptr(GBytes) random_cpio_data = NULL;

  /* We need to have /etc/passwd since dracut doesn't have altfiles
   * today.  Though maybe in the future we should add it, but
   * in the end we want to use systemd-sysusers of course.
   **/
  gboolean renamed_etc = FALSE;
  if (!rpmostree_core_undo_usretc (rootfs_dfd, &renamed_etc, error))
    return FALSE;
  gboolean have_passwd = FALSE;
  if (!rpmostree_passwd_prepare_rpm_layering (rootfs_dfd,
                                              NULL,
                                              &have_passwd,
                                              cancellable,
                                              error))
    return FALSE;

  /* Note rebuild_from_initramfs now is only used as a fallback in the client-side regen
   * path when we can't fetch the canonical initramfs args to use. */

  if (rebuild_from_initramfs)
    {
      rebuild_argv = g_ptr_array_new ();
      g_ptr_array_add (rebuild_argv, "--rebuild");
      g_ptr_array_add (rebuild_argv, (char*)rebuild_from_initramfs);

      /* In this case, any args specified in argv are *additional*
       * to the rebuild from the base.
       */
      for (char **iter = (char**)argv; iter && *iter; iter++)
        g_ptr_array_add (rebuild_argv, *iter);
      g_ptr_array_add (rebuild_argv, NULL);
      argv = (const char *const*)rebuild_argv->pdata;
    }

  /* First tempfile is just our shell script */
  if (!glnx_open_tmpfile_linkable_at (rootfs_dfd, "usr/bin",
                                      O_RDWR | O_CLOEXEC,
                                      &tmpf, error))
    goto out;
  if (glnx_loop_write (tmpf.fd, rpmostree_dracut_wrapper, strlen (rpmostree_dracut_wrapper)) < 0
      || fchmod (tmpf.fd, 0755) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE,
                             rootfs_dfd, rpmostree_dracut_wrapper_path,
                             error))
    goto out;
  /* Close the fd now, otherwise an exec will fail */
  glnx_tmpfile_clear (&tmpf);

  /* Second tempfile is the initramfs contents.  Note we generate the tmpfile
   * in . since in the current rpm-ostree design the temporary rootfs may not have tmp/
   * as a real mountpoint.
   */
  if (!glnx_open_tmpfile_linkable_at (rootfs_dfd, ".",
                                      O_RDWR | O_CLOEXEC,
                                      &tmpf, error))
    goto out;

  if (use_root_etc)
    {
      bwrap = rpmostree_bwrap_new_base (rootfs_dfd, error);
      if (!bwrap)
        return FALSE;
      rpmostree_bwrap_bind_read (bwrap, "/etc", "/etc");
      rpmostree_bwrap_bind_read (bwrap, "usr", "/usr");
    }
  else
    {
      bwrap = rpmostree_bwrap_new_base (rootfs_dfd, error);
      if (!bwrap)
        return FALSE;
      rpmostree_bwrap_bind_read (bwrap, "usr/etc", "/etc");
      rpmostree_bwrap_bind_read (bwrap, "usr", "/usr");
    }

  if (dracut_host_tmpdir)
    rpmostree_bwrap_bind_readwrite (bwrap, dracut_host_tmpdir->path, "/tmp/dracut");

  /* Set up argv and run */
  rpmostree_bwrap_append_child_argv (bwrap, (char*)glnx_basename (rpmostree_dracut_wrapper_path), NULL);
  for (char **iter = (char**)argv; iter && *iter; iter++)
    rpmostree_bwrap_append_child_argv (bwrap, *iter, NULL);

  if (kver)
    rpmostree_bwrap_append_child_argv (bwrap, "--kver", kver, NULL);

  rpmostree_bwrap_set_child_setup (bwrap, dracut_child_setup, GINT_TO_POINTER (tmpf.fd));

  if (!rpmostree_bwrap_run (bwrap, cancellable, error))
    goto out;

  /* For FIPS mode we need /dev/urandom pre-created because the FIPS
   * standards authors require that randomness is tested in a
   * *shared library constructor* (instead of first use as would be
   * the sane thing).
   * https://bugzilla.redhat.com/show_bug.cgi?id=1778940
   * https://bugzilla.redhat.com/show_bug.cgi?id=1401444
   * https://bugzilla.redhat.com/show_bug.cgi?id=1380866
   * */
  random_cpio_data = g_resources_lookup_data ("/rpmostree/dracut-random.cpio.gz",
                                              G_RESOURCE_LOOKUP_FLAGS_NONE,
                                              error);
  if (!random_cpio_data)
    return FALSE;
  gsize random_cpio_data_len = 0;
  const guint8* random_cpio_data_p = g_bytes_get_data (random_cpio_data, &random_cpio_data_len);
  if (lseek (tmpf.fd, 0, SEEK_END) < 0)
    return glnx_throw_errno_prefix (error, "lseek");
  if (glnx_loop_write (tmpf.fd, random_cpio_data_p, random_cpio_data_len) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (rebuild_from_initramfs)
    (void) unlinkat (rootfs_dfd, rebuild_from_initramfs, 0);

  if (have_passwd && !rpmostree_passwd_complete_rpm_layering (rootfs_dfd, error))
    goto out;

  if (renamed_etc && !rpmostree_core_redo_usretc (rootfs_dfd, error))
    goto out;

  ret = TRUE;
  *out_initramfs_tmpf = tmpf; tmpf.initialized = FALSE; /* Transfer */
 out:
  (void) unlinkat (rootfs_dfd, rpmostree_dracut_wrapper_path, 0);
  return ret;
}
