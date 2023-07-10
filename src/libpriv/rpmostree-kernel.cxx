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

#include <err.h>
#include <errno.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <libglnx.h>
#include <ostree.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include "rpmostree-core.h"
#include "rpmostree-cxxrs.h"
#include "rpmostree-kernel.h"
#include "rpmostree-util.h"

static const char usrlib_ostreeboot[] = "usr/lib/ostree-boot";

/* Keep this in sync with ostree/src/libostree/ostree-sysroot-deploy.c:get_kernel_from_tree().
 * Note they are of necessity slightly different since rpm-ostree needs
 * to support grabbing wherever the Fedora kernel RPM dropped files as well.
 */
static gboolean
find_kernel_and_initramfs_in_bootdir (int rootfs_dfd, const char *bootdir, char **out_ksuffix,
                                      char **out_kernel, char **out_initramfs,
                                      GCancellable *cancellable, GError **error)
{
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
  glnx_autofd int dfd = -1;
  g_autofree char *ret_ksuffix = NULL;
  g_autofree char *ret_kernel = NULL;
  g_autofree char *ret_initramfs = NULL;

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

      if (out_ksuffix ? g_str_has_prefix (name, "vmlinuz-") : g_str_equal (name, "vmlinuz"))
        {
          if (ret_kernel)
            return glnx_throw (error, "Multiple vmlinuz%s in %s, occurrences '%s' and '%s/%s'",
                               out_ksuffix ? "-" : "", bootdir, ret_kernel, bootdir, name);
          if (out_ksuffix)
            ret_ksuffix = g_strdup (name + strlen ("vmlinuz-"));
          ret_kernel = g_strconcat (bootdir, "/", name, NULL);
        }
      else if (g_str_equal (name, "initramfs.img") || g_str_has_prefix (name, "initramfs-"))
        {
          if (ret_initramfs)
            return glnx_throw (error, "Multiple initramfs- in %s, occurrences '%s' and '%s/%s'",
                               bootdir, ret_initramfs, bootdir, name);
          ret_initramfs = g_strconcat (bootdir, "/", name, NULL);
        }
    }

  if (out_ksuffix)
    *out_ksuffix = util::move_nullify (ret_ksuffix);
  *out_kernel = util::move_nullify (ret_kernel);
  *out_initramfs = util::move_nullify (ret_initramfs);
  return TRUE;
}

/* Given a directory @subpath, find the first child that is a directory
 * and contains a `vmlinuz` file, returning it in @out_subdir.  If there are multiple directories,
 * return an error.
 */
static gboolean
find_dir_with_vmlinuz (int rootfs_dfd, const char *subpath, char **out_subdir,
                       GCancellable *cancellable, GError **error)
{
  g_autofree char *ret_subdir = NULL;
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };

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
      // Ignore directories without vmlinuz
      struct stat stbuf;
      g_autofree char *vmlinuz_path = g_strdup_printf ("%s/vmlinuz", dent->d_name);
      if (!glnx_fstatat_allow_noent (dfd_iter.fd, vmlinuz_path, &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == ENOENT)
        continue;

      if (ret_subdir)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Multiple kernels (vmlinuz) found in: %s: %s %s", subpath,
                       glnx_basename (ret_subdir), dent->d_name);
          return FALSE;
        }
      ret_subdir = g_strconcat (subpath, "/", dent->d_name, NULL);
    }

  *out_subdir = util::move_nullify (ret_subdir);
  return TRUE;
}

static gboolean
kernel_remove_in (int rootfs_dfd, const char *bootdir, gboolean has_ksuffix,
                  GCancellable *cancellable, GError **error)
{
  g_autofree char *ksuffix = NULL;
  g_autofree char *kernel_path = NULL;
  g_autofree char *initramfs_path = NULL;
  if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir, has_ksuffix ? &ksuffix : NULL,
                                             &kernel_path, &initramfs_path, cancellable, error))
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
rpmostree_kernel_remove (int rootfs_dfd, GCancellable *cancellable, GError **error)
{
  g_autofree char *modversion_dir = NULL;
  if (!find_dir_with_vmlinuz (rootfs_dfd, "usr/lib/modules", &modversion_dir, cancellable, error))
    return FALSE;
  if (modversion_dir)
    {
      if (!kernel_remove_in (rootfs_dfd, modversion_dir, FALSE, cancellable, error))
        return FALSE;
      glnx_autofd int modversion_dfd = -1;
      if (!glnx_opendirat (rootfs_dfd, modversion_dir, TRUE, &modversion_dfd, error))
        return FALSE;
      /* Source of truth of file list is
       * https://git.kernel.org/pub/scm/utils/kernel/kmod/kmod.git/tree/tools/depmod.c?id=43bdf97ce1298c8727effb470291ed884e1161e6#n2477
       *
       * TODO: Add a depmod --clean <kver> command.
       *
       * See also `/usr/lib/kernel/install.d/50-depmod.install`
       * which is run by `kernel-install remove` from RPM `%postun`.
       */
      const char *depmod_files[]
          = { "modules.alias",       "modules.alias.bin", "modules.builtin.alias.bin",
              "modules.builtin.bin", "modules.dep",       "modules.dep.bin",
              "modules.devname",     "modules.softdep",   "modules.symbols",
              "modules.symbols.bin" };
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
  if (!kernel_remove_in (rootfs_dfd, "usr/lib/ostree-boot", TRUE, cancellable, error))
    return FALSE;
  if (!kernel_remove_in (rootfs_dfd, "boot", TRUE, cancellable, error))
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
rpmostree_find_kernel (int rootfs_dfd, GCancellable *cancellable, GError **error)
{
  /* First, look for the kernel in the canonical ostree directory */
  g_autofree char *kver = NULL;
  g_autofree char *kernel_path = NULL;
  g_autofree char *initramfs_path = NULL;
  g_autofree char *bootdir = g_strdup (usrlib_ostreeboot);
  if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir, &kver, &kernel_path,
                                             &initramfs_path, cancellable, error))
    return NULL;

  /* Next, the traditional /boot */
  if (kernel_path == NULL)
    {
      g_free (bootdir);
      bootdir = g_strdup ("boot");

      if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir, &kver, &kernel_path,
                                                 &initramfs_path, cancellable, error))
        return NULL;
    }

  /* Strip off the digest if we got it from /boot or /usr/lib/ostree-boot */
  if (kernel_path != NULL)
    {
      const size_t n = strlen (kver);
      if (n > 65 && kver[n - 65] == '-' && ostree_validate_checksum_string (kver + n - 64, NULL))
        kver[n - 65] = '\0';
    }

  /* Finally, the newer model of having the kernel with the modules */
  if (kernel_path == NULL)
    {
      /* Fetch the kver from /usr/lib/modules */
      g_autofree char *modversion_dir = NULL;
      if (!find_dir_with_vmlinuz (rootfs_dfd, "usr/lib/modules", &modversion_dir, cancellable,
                                  error))
        return NULL;

      if (!modversion_dir)
        return (GVariant *)glnx_null_throw (error, "/usr/lib/modules is empty");

      kver = g_strdup (glnx_basename (modversion_dir));

      g_free (bootdir);
      bootdir = util::move_nullify (modversion_dir);
      if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir, NULL, &kernel_path,
                                                 &initramfs_path, cancellable, error))
        return NULL;
    }

  if (kernel_path == NULL)
    return (GVariant *)glnx_null_throw (error,
                                        "Unable to find kernel (vmlinuz) in /boot "
                                        "or /usr/lib/modules (bootdir=%s)",
                                        bootdir);

  return g_variant_ref_sink (g_variant_new ("(sssms)", kver, bootdir, kernel_path, initramfs_path));
}

/* Given a @rootfs_dfd and path to kernel/initramfs that live in
 * usr/lib/modules/$kver, possibly update @bootdir to use them. @bootdir should
 * be one of either /usr/lib/ostree-boot or /boot. If @only_if_found is set, we
 * do the copy only if we find a kernel; this way we avoid e.g. touching /boot
 * if it isn't being used.
 */
static gboolean
copy_kernel_into (int rootfs_dfd, const char *kver, const char *boot_checksum_str,
                  const char *kernel_modules_path, const char *initramfs_modules_path,
                  gboolean only_if_found, const char *bootdir, GCancellable *cancellable,
                  GError **error)
{
  g_autofree char *legacy_kernel_suffix = NULL;
  g_autofree char *legacy_kernel_path = NULL;
  g_autofree char *legacy_initramfs_path = NULL;
  if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir, &legacy_kernel_suffix,
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
  legacy_initramfs_path
      = g_strconcat (bootdir, "/", "initramfs-", kver, ".img-", boot_checksum_str, NULL);
  if (linkat (rootfs_dfd, initramfs_modules_path, rootfs_dfd, legacy_initramfs_path, 0) < 0)
    return glnx_throw_errno_prefix (error, "linkat(%s)", legacy_initramfs_path);

  return TRUE;
}

/* Given a kernel path and a temporary initramfs, place them in their final
 * location. We handle /usr/lib/modules as well as the /usr/lib/ostree-boot and
 * /boot paths where we need to pre-compute their checksum.
 */
gboolean
rpmostree_finalize_kernel (int rootfs_dfd, const char *bootdir, const char *kver,
                           const char *kernel_path, GLnxTmpfile *initramfs_tmpf,
                           RpmOstreeFinalizeKernelDestination dest, GCancellable *cancellable,
                           GError **error)
{
  const char slash_bootdir[] = "boot";
  g_autofree char *modules_bootdir = g_build_filename ("usr/lib/modules", kver, NULL);

  /* Calculate the sha256sum of the kernel+initramfs (called the "boot
   * checksum"). We checksum the initramfs from the tmpfile fd (via mmap()) to
   * avoid writing it to disk in another temporary location.
   */
  g_autoptr (GChecksum) boot_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (!_rpmostree_util_update_checksum_from_file (boot_checksum, rootfs_dfd, kernel_path,
                                                  cancellable, error))
    return FALSE;

  g_autofree char *initramfs_modules_path
      = g_build_filename (modules_bootdir, "initramfs.img", NULL);

  if (initramfs_tmpf && initramfs_tmpf->initialized)
    {
      g_autoptr (GMappedFile) mfile = g_mapped_file_new_from_fd (initramfs_tmpf->fd, FALSE, error);
      if (!mfile)
        return glnx_prefix_error (error, "mmap(initramfs)");
      g_checksum_update (boot_checksum, (guint8 *)g_mapped_file_get_contents (mfile),
                         g_mapped_file_get_length (mfile));

      /* Replace the initramfs */
      if (unlinkat (rootfs_dfd, initramfs_modules_path, 0) < 0)
        {
          if (errno != ENOENT)
            return glnx_throw_errno_prefix (error, "unlinkat(%s)", initramfs_modules_path);
        }
      if (!glnx_link_tmpfile_at (initramfs_tmpf, GLNX_LINK_TMPFILE_NOREPLACE, rootfs_dfd,
                                 initramfs_modules_path, error))
        return glnx_prefix_error (error, "Linking initramfs");
    }
  else
    {
      /* we're not replacing the initramfs; use built-in one */
      if (!_rpmostree_util_update_checksum_from_file (boot_checksum, rootfs_dfd,
                                                      initramfs_modules_path, cancellable, error))
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

  ROSCXX_TRY (verify_kernel_hmac (rootfs_dfd, rust::Str (modules_bootdir)), error);

  /* Update /usr/lib/ostree-boot and /boot (if desired) */
  const gboolean only_if_found = (dest == RPMOSTREE_FINALIZE_KERNEL_AUTO);
  if (only_if_found || dest >= RPMOSTREE_FINALIZE_KERNEL_USRLIB_OSTREEBOOT)
    {
      if (!copy_kernel_into (rootfs_dfd, kver, boot_checksum_str, kernel_modules_path,
                             initramfs_modules_path, only_if_found, usrlib_ostreeboot, cancellable,
                             error))
        return FALSE;
    }
  if (only_if_found || dest >= RPMOSTREE_FINALIZE_KERNEL_SLASH_BOOT)
    {
      if (!copy_kernel_into (rootfs_dfd, kver, boot_checksum_str, kernel_modules_path,
                             initramfs_modules_path, only_if_found, slash_bootdir, cancellable,
                             error))
        return FALSE;
    }
  return TRUE;
}

struct Unlinker
{
  int rootfs_dfd;
  const char *path;

  ~Unlinker () { (void)unlinkat (rootfs_dfd, path, 0); }
};

gboolean
rpmostree_run_dracut (int rootfs_dfd, const char *const *argv, const char *kver,
                      const char *rebuild_from_initramfs, gboolean use_root_etc,
                      GLnxTmpDir *dracut_host_tmpdir, GLnxTmpfile *out_initramfs_tmpf,
                      GCancellable *cancellable, GError **error)
{
  auto destdir = rpmostreecxx::cliwrap_destdir ();
  /* Shell wrapper around dracut to write to the O_TMPFILE fd;
   * at some point in the future we should add --fd X instead of -f
   * to dracut.
   */
  static const char rpmostree_dracut_wrapper_path[] = "usr/bin/rpmostree-dracut-wrapper";
  /* This also hardcodes a few arguments */
  g_autofree char *rpmostree_dracut_wrapper
      = g_strdup_printf ("#!/usr/bin/bash\n"
                         "set -euo pipefail\n"
                         "export PATH=%s:${PATH}\n"
                         "extra_argv=; if (dracut --help; true) | grep -q -e --reproducible; then "
                         "extra_argv=\"--reproducible\"; fi\n"
                         "mkdir -p /tmp/dracut && dracut $extra_argv -v --add ostree "
                         "--tmpdir=/tmp/dracut -f /tmp/initramfs.img \"$@\"\n"
                         "cat /tmp/initramfs.img >/proc/self/fd/3\n",
                         destdir.c_str ());
  g_autoptr (GPtrArray) rebuild_argv = NULL;
  g_auto (GLnxTmpfile) tmpf = {
    0,
  };

  /* We need to have /etc/passwd since dracut doesn't have altfiles
   * today.  Though maybe in the future we should add it, but
   * in the end we want to use systemd-sysusers of course.
   **/
  CXX_TRY_VAR (etc_guard, rpmostreecxx::prepare_tempetc_guard (rootfs_dfd), error);

  CXX_TRY_VAR (have_passwd, rpmostreecxx::prepare_rpm_layering (rootfs_dfd, ""), error);

  /* Note rebuild_from_initramfs now is only used as a fallback in the client-side regen
   * path when we can't fetch the canonical initramfs args to use. */

  if (rebuild_from_initramfs)
    {
      rebuild_argv = g_ptr_array_new ();
      g_ptr_array_add (rebuild_argv, (char *)"--rebuild");
      g_ptr_array_add (rebuild_argv, (char *)rebuild_from_initramfs);

      /* In this case, any args specified in argv are *additional*
       * to the rebuild from the base.
       */
      for (char **iter = (char **)argv; iter && *iter; iter++)
        g_ptr_array_add (rebuild_argv, *iter);
      g_ptr_array_add (rebuild_argv, NULL);
      argv = (const char *const *)rebuild_argv->pdata;
    }

  /* First tempfile is just our shell script */
  if (!glnx_open_tmpfile_linkable_at (rootfs_dfd, "usr/bin", O_RDWR | O_CLOEXEC, &tmpf, error))
    return FALSE;
  if (glnx_loop_write (tmpf.fd, rpmostree_dracut_wrapper, strlen (rpmostree_dracut_wrapper)) < 0
      || fchmod (tmpf.fd, 0755) < 0)
    return glnx_throw_errno_prefix (error, "writing");

  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE, rootfs_dfd,
                             rpmostree_dracut_wrapper_path, error))
    return FALSE;
  /* Close the fd now, otherwise an exec will fail */
  glnx_tmpfile_clear (&tmpf);

  auto unlinker = Unlinker{ .rootfs_dfd = rootfs_dfd, .path = rpmostree_dracut_wrapper_path };

  /* Second tempfile is the initramfs contents.  Note we generate the tmpfile
   * in . since in the current rpm-ostree design the temporary rootfs may not have tmp/
   * as a real mountpoint.
   */
  if (!glnx_open_tmpfile_linkable_at (rootfs_dfd, ".", O_RDWR | O_CLOEXEC, &tmpf, error))
    return FALSE;

  CXX_TRY_VAR (bwrap, rpmostreecxx::bubblewrap_new (rootfs_dfd), error);
  if (use_root_etc)
    {
      bwrap->bind_read ("/etc", "/etc");
      bwrap->bind_read ("usr", "/usr");
    }
  else
    {
      bwrap->bind_read ("usr/etc", "/etc");
      bwrap->bind_read ("usr", "/usr");
    }

  /* The only baked special files we need are /dev/[u]random (see below). So tell dracut to not even
   * try to create the other ones (/dev/null, /dev/kmsg, and /dev/console) since it'll fail anyway
   * (and print ugly messages) since we don't give it `CAP_MKNOD`. */
  bwrap->setenv ("DRACUT_NO_MKNOD", "1");

  if (dracut_host_tmpdir)
    bwrap->bind_readwrite (dracut_host_tmpdir->path, "/tmp/dracut");

  /* Set up argv and run */
  bwrap->append_child_arg ((const char *)glnx_basename (rpmostree_dracut_wrapper_path));
  for (char **iter = (char **)argv; iter && *iter; iter++)
    bwrap->append_child_arg (*iter);

  if (kver)
    {
      bwrap->append_child_arg ("--kver");
      bwrap->append_child_arg (kver);
    }

  // Pass the tempfile to the child as fd 3
  glnx_autofd int tmpf_child = fcntl (tmpf.fd, F_DUPFD_CLOEXEC, 3);
  if (tmpf_child < 0)
    return glnx_throw_errno_prefix (error, "fnctl");
  bwrap->take_fd (glnx_steal_fd (&tmpf_child), 3);

  CXX_TRY (bwrap->run (*cancellable), error);

  /* For FIPS mode we need /dev/urandom pre-created because the FIPS
   * standards authors require that randomness is tested in a
   * *shared library constructor* (instead of first use as would be
   * the sane thing).
   * https://bugzilla.redhat.com/show_bug.cgi?id=1778940
   * https://bugzilla.redhat.com/show_bug.cgi?id=1401444
   * https://bugzilla.redhat.com/show_bug.cgi?id=1380866
   * */
  auto random_cpio_data = rpmostreecxx::get_dracut_random_cpio ();
  if (lseek (tmpf.fd, 0, SEEK_END) < 0)
    return glnx_throw_errno_prefix (error, "lseek");
  if (glnx_loop_write (tmpf.fd, random_cpio_data.data (), random_cpio_data.length ()) < 0)
    return glnx_throw_errno_prefix (error, "write");

  if (rebuild_from_initramfs)
    (void)unlinkat (rootfs_dfd, rebuild_from_initramfs, 0);

  if (have_passwd)
    ROSCXX_TRY (complete_rpm_layering (rootfs_dfd), error);

  CXX_TRY (etc_guard->undo (), error);

  *out_initramfs_tmpf = tmpf;
  tmpf.initialized = FALSE; /* Transfer */
  return TRUE;
}
