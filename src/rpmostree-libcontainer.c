/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <string.h>
#include <glib-unix.h>
#include <sys/mount.h>
#include <json-glib/json-glib.h>
#include <gio/gunixoutputstream.h>

#include "rpmostree-util.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-libcontainer.h"
#include "libgsystem.h"

gboolean
rpmostree_container_bind_mount_readonly (const char *path, GError **error)
{
  gboolean ret = FALSE;

  if (mount (path, path, NULL, MS_BIND | MS_PRIVATE, NULL) != 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "mount(%s, MS_BIND): %s",
                   path,
                   g_strerror (errsv));
      goto out;
    }
  if (mount (path, path, NULL, MS_BIND | MS_PRIVATE | MS_REMOUNT | MS_RDONLY, NULL) != 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "mount(%s, MS_BIND | MS_RDONLY): %s",
                   path,
                   g_strerror (errsv));
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}


/* Based on code from nspawn.c */
int
_rpmostree_libcontainer_make_api_mounts (const char *dest)
{
  typedef struct MountPoint {
    const char *what;
    const char *where;
    const char *type;
    const char *options;
    unsigned long flags;
    gboolean fatal;
  } MountPoint;

  static const MountPoint mount_table[] = {
    { "proc",      "/proc",     "proc",  NULL,        MS_NOSUID|MS_NOEXEC|MS_NODEV,           TRUE  },
    { "/proc/sys", "/proc/sys", NULL,    NULL,        MS_BIND,                                TRUE  },   /* Bind mount first */
    { NULL,        "/proc/sys", NULL,    NULL,        MS_BIND|MS_RDONLY|MS_REMOUNT,           TRUE  },   /* Then, make it r/o */
    { "sysfs",     "/sys",      "sysfs", NULL,        MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV, TRUE  },
    { "tmpfs",     "/dev",      "tmpfs", "mode=755",  MS_NOSUID|MS_STRICTATIME,               TRUE  },
    { "devpts",    "/dev/pts",  "devpts","newinstance,ptmxmode=0666,mode=620,gid=5", MS_NOSUID|MS_NOEXEC, TRUE },
    { "tmpfs",     "/dev/shm",  "tmpfs", "mode=1777", MS_NOSUID|MS_NODEV|MS_STRICTATIME,      TRUE  },
    { "tmpfs",     "/run",      "tmpfs", "mode=755",  MS_NOSUID|MS_NODEV|MS_STRICTATIME,      TRUE  },
    { "/sys/fs/selinux", "/sys/fs/selinux", NULL, NULL, MS_BIND,                              FALSE },  /* Bind mount first */
    { NULL,              "/sys/fs/selinux", NULL, NULL, MS_BIND|MS_RDONLY|MS_REMOUNT,         FALSE },  /* Then, make it r/o */
  };

  unsigned k;

  for (k = 0; k < G_N_ELEMENTS(mount_table); k++)
    {
      gs_free char *where = NULL;
      gs_free char *options = NULL;
      const char *o;
      int t;

      where = g_build_filename (dest, "/", mount_table[k].where, NULL);

      t = mkdir (where, 0755);
      if (t < 0 && errno != EEXIST)
        {
          if (!mount_table[k].fatal)
            continue;
          return -1;
        }

      o = mount_table[k].options;

      if (mount (mount_table[k].what,
                 where,
                 mount_table[k].type,
                 mount_table[k].flags,
                 o) < 0)
        {
          if (errno == ENOENT && !mount_table[k].fatal)
            continue;
          return -1;
        }
    }

  return 0;
}

int
_rpmostree_libcontainer_prep_dev (const char  *dest_devdir)
{
  _cleanup_close_ int src_fd = -1;
  _cleanup_close_ int dest_fd = -1;
  struct stat stbuf;
  guint i;
  static const char *const devnodes[] = { "null", "zero", "full", "random", "urandom", "tty" };

  src_fd = openat (AT_FDCWD, "/dev", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (src_fd == -1)
    return -1;

  dest_fd = openat (AT_FDCWD, dest_devdir, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (dest_fd == -1)
    return -1;

  for (i = 0; i < G_N_ELEMENTS (devnodes); i++)
    {
      const char *nodename = devnodes[i];
      
      if (fstatat (src_fd, nodename, &stbuf, 0) == -1)
        {
          if (errno == ENOENT)
            continue;
          else
            return -1;
        }

      if (mknodat (dest_fd, nodename, stbuf.st_mode, stbuf.st_rdev) != 0)
        return -1;
      if (fchmodat (dest_fd, nodename, stbuf.st_mode, 0) != 0)
        return -1;
    }

  return 0;
}

pid_t
_rpmostree_libcontainer_run_in_root (const char  *dest,
                                     const char  *binary,
                                     char **argv)
{
  const int cloneflags = 
    SIGCHLD | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | CLONE_SYSVSEM | CLONE_NEWUTS;
  pid_t child;

  if ((child = syscall (__NR_clone, cloneflags, NULL)) < 0)
    return -1;

  if (child != 0)
    return child;

  if (mount (NULL, "/", "none", MS_PRIVATE | MS_REC, NULL) != 0)
    _rpmostree_perror_fatal ("mount: ");
    
  if (mount (NULL, "/", "none", MS_PRIVATE | MS_REMOUNT | MS_NOSUID, NULL) != 0)
    _rpmostree_perror_fatal ("mount (MS_NOSUID): ");

  if (chdir (dest) != 0)
    _rpmostree_perror_fatal ("chdir: ");

  if (_rpmostree_libcontainer_make_api_mounts (dest) != 0)
    _rpmostree_perror_fatal ("preparing api mounts: ");

  if (_rpmostree_libcontainer_prep_dev ("dev") != 0)
    _rpmostree_perror_fatal ("preparing /dev: ");

  if (mount (".", ".", NULL, MS_BIND | MS_PRIVATE, NULL) != 0)
    _rpmostree_perror_fatal ("mount (MS_BIND)");

  if (mount (dest, "/", NULL, MS_MOVE, NULL) != 0)
    _rpmostree_perror_fatal ("mount (MS_MOVE)");

  if (chroot (".") != 0)
    _rpmostree_perror_fatal ("chroot: ");

  if (chdir ("/") != 0)
    _rpmostree_perror_fatal ("chdir: ");

  if (execv (binary, argv) != 0)
    _rpmostree_perror_fatal ("execl: ");

  g_assert_not_reached ();
}
