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

#include "rpmostree-kernel.h"
#include "rpmostree-bwrap.h"
#include "rpmostree-util.h"

static gboolean
find_kernel_and_initramfs_in_bootdir (int          rootfs_dfd,
                                      const char  *bootdir,
                                      char       **out_kernel,
                                      char       **out_initramfs,
                                      GCancellable *cancellable,
                                      GError     **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  glnx_fd_close int dfd = -1;
  g_autofree char* ret_kernel = NULL;
  g_autofree char* ret_initramfs = NULL;

  *out_kernel = *out_initramfs = NULL;

  dfd = glnx_opendirat_with_errno (rootfs_dfd, bootdir, FALSE);
  if (dfd < 0)
    {
      if (errno == ENOENT)
        return TRUE;
      else
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }
  if (!glnx_dirfd_iterator_init_take_fd (dfd, &dfd_iter, error))
    return FALSE;
  dfd = -1;

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
      if (strcmp (name, "vmlinuz") == 0 || g_str_has_prefix (name, "vmlinuz-"))
        {
          if (ret_kernel)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Multiple vmlinuz- in %s",
                           bootdir);
              return FALSE;
            }
          ret_kernel = g_strconcat (bootdir, "/", name, NULL);
        }
      else if (g_str_has_prefix (name, "initramfs-"))
        {
          if (ret_initramfs)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Multiple initramfs- in %s", bootdir);
              return FALSE;
            }
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

/* Given a root filesystem, return a GVariant of format (sssms):
 *  - kver: uname -r equivalent
 *  - bootdir: Path to the boot directory
 *  - kernel_path: Relative path to kernel
 *  - initramfs_path: Relative path to initramfs (may be NULL if no initramfs)
 */
GVariant *
rpmostree_find_kernel (int rootfs_dfd,
                       GCancellable *cancellable,
                       GError **error)
{
  g_autofree char* kernel_path = NULL;
  g_autofree char* initramfs_path = NULL;
  const char *kver = NULL; /* May point to kver_owned */
  g_autofree char *kver_owned = NULL;
  g_autofree char *bootdir = g_strdup ("usr/lib/ostree-boot");

  /* First, look in the canonical ostree directory */
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
      g_autofree char* modversion_dir = NULL;

      if (!find_ensure_one_subdirectory (rootfs_dfd, "usr/lib/modules", &modversion_dir,
                                         cancellable, error))
        return NULL;

      if (modversion_dir)
        {
          kver = glnx_basename (modversion_dir);
          bootdir = g_steal_pointer (&modversion_dir);
          if (!find_kernel_and_initramfs_in_bootdir (rootfs_dfd, bootdir,
                                                     &kernel_path, &initramfs_path,
                                                     cancellable, error))
            return NULL;
        }
    }

  if (kernel_path == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to find kernel (vmlinuz) in /boot or /usr/lib/modules (bootdir=%s)", bootdir);
      return NULL;
    }

  if (!kver)
    {
      const char *kname = glnx_basename (kernel_path);
      const char *kver_p;

      kver_p = strchr (kname, '-');
      g_assert (kver_p);
      kver = kver_owned = g_strdup (kver_p + 1);
    }

  return g_variant_ref_sink (g_variant_new ("(sssms)", kver, bootdir, kernel_path, initramfs_path));
}

/* Given a kernel path and a temporary initramfs, compute their checksum and put
 * them in their final locations.
 */
gboolean
rpmostree_finalize_kernel (int rootfs_dfd,
                           const char *bootdir,
                           const char *kver,
                           const char *kernel_path,
                           const char *initramfs_tmp_path,
                           int         initramfs_tmp_fd,
                           GCancellable *cancellable,
                           GError **error)
{
  g_autoptr(GChecksum) boot_checksum = NULL;
  g_autofree char *kernel_final_path = NULL;
  g_autofree char *initramfs_final_path = NULL;
  const char *boot_checksum_str = NULL;

  /* Now, calculate the combined sha256sum of the two. We checksum the initramfs
   * from the tmpfile fd (via mmap()) to avoid writing it to disk in another
   * temporary location.
   */
  boot_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (!_rpmostree_util_update_checksum_from_file (boot_checksum, rootfs_dfd, kernel_path,
                                                  cancellable, error))
    return FALSE;

  { g_autoptr(GMappedFile) mfile = g_mapped_file_new_from_fd (initramfs_tmp_fd, FALSE, error);
    if (!mfile)
      return FALSE;
    g_checksum_update (boot_checksum, (guint8*)g_mapped_file_get_contents (mfile),
                       g_mapped_file_get_length (mfile));
  }
  boot_checksum_str = g_checksum_get_string (boot_checksum);

  kernel_final_path = g_strconcat (bootdir, "/", glnx_basename (kernel_path), "-", boot_checksum_str, NULL);
  initramfs_final_path = g_strconcat (bootdir, "/", "initramfs-", kver, ".img-", boot_checksum_str, NULL);

  /* Put the kernel in the final location */
  if (renameat (rootfs_dfd, kernel_path, rootfs_dfd, kernel_final_path) < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }
  /* Link the initramfs directly to its final destination */
  if (!glnx_link_tmpfile_at (rootfs_dfd, GLNX_LINK_TMPFILE_NOREPLACE,
                             initramfs_tmp_fd, initramfs_tmp_path,
                             rootfs_dfd, initramfs_final_path,
                             error))
    return FALSE;

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
                      char   **argv,
                      const char *rebuild_from_initramfs,
                      int     *out_initramfs_tmpfd,
                      char   **out_initramfs_tmppath,
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
  static const char rpmostree_dracut_wrapper[] =
    "#!/usr/bin/bash\n"
    "set -euo pipefail\n"
    "extra_argv=; if (dracut --help; true) | grep -q -e --reproducible; then extra_argv=\"--reproducible --gzip\"; fi\n"
    "dracut $extra_argv -v --add ostree --tmpdir=/tmp -f /tmp/initramfs.img \"$@\"\n"
    "cat /tmp/initramfs.img >/proc/self/fd/3\n";
  glnx_fd_close int tmp_fd = -1;
  g_autofree char *tmpfile_path = NULL;
  g_autoptr(RpmOstreeBwrap) bwrap = NULL;
  g_autoptr(GPtrArray) rebuild_argv = NULL;

  g_assert (argv != NULL || rebuild_from_initramfs != NULL);

  if (rebuild_from_initramfs)
    {
      rebuild_argv = g_ptr_array_new ();
      g_ptr_array_add (rebuild_argv, "--rebuild");
      g_ptr_array_add (rebuild_argv, (char*)rebuild_from_initramfs);
      /* In this case, any args specified in argv are *additional*
       * to the rebuild from the base.
       */
      for (char **iter = argv; iter && *iter; iter++)
        g_ptr_array_add (rebuild_argv, *iter);
      g_ptr_array_add (rebuild_argv, NULL);
      argv = (char**)rebuild_argv->pdata;
    }

  /* First tempfile is just our shell script */
  if (!glnx_open_tmpfile_linkable_at (rootfs_dfd, "usr/bin",
                                      O_RDWR | O_CLOEXEC,
                                      &tmp_fd, &tmpfile_path,
                                      error))
    goto out;
  if (glnx_loop_write (tmp_fd, rpmostree_dracut_wrapper, sizeof (rpmostree_dracut_wrapper)) < 0
      || fchmod (tmp_fd, 0755) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  
  if (!glnx_link_tmpfile_at (rootfs_dfd, GLNX_LINK_TMPFILE_NOREPLACE,
                             tmp_fd, tmpfile_path, rootfs_dfd, rpmostree_dracut_wrapper_path,
                             error))
    goto out;
  /* We need to close the writable FD now to be able to exec it */
  close (tmp_fd); tmp_fd = -1;

  /* Second tempfile is the initramfs contents.  Note we generate the tmpfile
   * in . since in the current rpm-ostree design the temporary rootfs may not have tmp/
   * as a real mountpoint.
   */
  if (!glnx_open_tmpfile_linkable_at (rootfs_dfd, ".",
                                      O_RDWR | O_CLOEXEC,
                                      &tmp_fd, &tmpfile_path,
                                      error))
    goto out;

  if (rebuild_from_initramfs)
    bwrap = rpmostree_bwrap_new (rootfs_dfd, RPMOSTREE_BWRAP_IMMUTABLE, error,
                                 "--ro-bind", "/etc", "/etc",
                                 NULL);
  else
    bwrap = rpmostree_bwrap_new (rootfs_dfd, RPMOSTREE_BWRAP_IMMUTABLE, error,
                                 NULL);
  if (!bwrap)
    return FALSE;

  /* Set up argv and run */
  rpmostree_bwrap_append_child_argv (bwrap, (char*)glnx_basename (rpmostree_dracut_wrapper_path), NULL);
  for (char **iter = argv; iter && *iter; iter++)
    rpmostree_bwrap_append_child_argv (bwrap, *iter, NULL);

  rpmostree_bwrap_set_child_setup (bwrap, dracut_child_setup, GINT_TO_POINTER (tmp_fd));

  if (!rpmostree_bwrap_run (bwrap, error))
    goto out;

  if (rebuild_from_initramfs)
    (void) unlinkat (rootfs_dfd, rebuild_from_initramfs, 0);

  ret = TRUE;
  *out_initramfs_tmpfd = tmp_fd; tmp_fd = -1;
  *out_initramfs_tmppath = g_steal_pointer (&tmpfile_path);
 out:
  if (tmpfile_path != NULL)
    (void) unlink (tmpfile_path);
  unlinkat (rootfs_dfd, rpmostree_dracut_wrapper_path, 0);
  return ret;
}
