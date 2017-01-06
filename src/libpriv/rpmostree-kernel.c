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

  /* Second tempfile is the initramfs contents */
  if (!glnx_open_tmpfile_linkable_at (rootfs_dfd, "tmp",
                                      O_WRONLY | O_CLOEXEC,
                                      &tmp_fd, &tmpfile_path,
                                      error))
    goto out;

  bwrap = rpmostree_bwrap_new (rootfs_dfd, RPMOSTREE_BWRAP_IMMUTABLE, error, NULL);
  if (!bwrap)
    return FALSE;

  /* Set up argv and run */
  rpmostree_bwrap_append_child_argv (bwrap, (char*)glnx_basename (rpmostree_dracut_wrapper_path), NULL);
  for (char **iter = argv; iter && *iter; iter++)
    rpmostree_bwrap_append_child_argv (bwrap, *iter, NULL);

  rpmostree_bwrap_set_child_setup (bwrap, dracut_child_setup, GINT_TO_POINTER (tmp_fd));

  if (!rpmostree_bwrap_run (bwrap, error))
    goto out;

  ret = TRUE;
  *out_initramfs_tmpfd = tmp_fd; tmp_fd = -1;
  *out_initramfs_tmppath = g_steal_pointer (&tmpfile_path);
 out:
  if (tmpfile_path != NULL)
    (void) unlink (tmpfile_path);
  unlinkat (rootfs_dfd, rpmostree_dracut_wrapper_path, 0);
  return ret;
}
