/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Red Hat, Inc.
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
#include "rpmostree-bwrap.h"

#include <err.h>

void
rpmostree_ptrarray_append_strdup (GPtrArray *argv_array, ...)
{
  va_list args;
  char *arg;

  va_start (args, argv_array);
  while ((arg = va_arg (args, char *)))
    g_ptr_array_add (argv_array, g_strdup (arg));
  va_end (args);
}

GPtrArray *
rpmostree_bwrap_base_argv_new_for_rootfs (int rootfs_fd, GError **error)
{
  g_autoptr(GPtrArray) bwrap_argv = g_ptr_array_new_with_free_func (g_free);
  static const char *usr_links[] = {"lib", "lib32", "lib64", "bin", "sbin"};

  rpmostree_ptrarray_append_strdup (bwrap_argv,
                                    WITH_BUBBLEWRAP_PATH,
                                    "--dev", "/dev",
                                    "--proc", "/proc",
                                    "--dir", "/tmp",
                                    "--chdir", "/",
                                    "--ro-bind", "/sys/block", "/sys/block",
                                    "--ro-bind", "/sys/bus", "/sys/bus",
                                    "--ro-bind", "/sys/class", "/sys/class",
                                    "--ro-bind", "/sys/dev", "/sys/dev",
                                    "--ro-bind", "/sys/devices", "/sys/devices",
                                    NULL);

  for (guint i = 0; i < G_N_ELEMENTS (usr_links); i++)
    {
      const char *subdir = usr_links[i];
      struct stat stbuf;
      char *path;

      if (fstatat (rootfs_fd, subdir, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
        {
          if (errno != ENOENT)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
          continue;
        }
      else if (!S_ISLNK (stbuf.st_mode))
        continue;

      g_ptr_array_add (bwrap_argv, g_strdup ("--symlink"));

      path = g_strconcat ("usr/", subdir, NULL);
      g_ptr_array_add (bwrap_argv, path);

      path = g_strconcat ("/", subdir, NULL);
      g_ptr_array_add (bwrap_argv, path);
    }

  return g_steal_pointer (&bwrap_argv);
}

static void
child_setup_fchdir (gpointer user_data)
{
  int fd = GPOINTER_TO_INT (user_data);
  if (fchdir (fd) < 0)
    err (1, "fchdir");
}

gboolean
rpmostree_run_sync_fchdir_setup (char **argv_array, GSpawnFlags flags,
                                 int rootfs_fd, GError **error)
{
  int estatus;
  
  if (!g_spawn_sync (NULL, argv_array, NULL, flags,
                     child_setup_fchdir, GINT_TO_POINTER (rootfs_fd),
                     NULL, NULL, &estatus, error))
    return FALSE;
  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;

  return TRUE;
}

/* mock doesn't actually use a mount namespace, and hence bwrap will
 * fail to remount /.  Work around this by doing it here.  Fedora
 * runs rpm-ostree inside of mock instead of Docker or something
 * more modern.
 */
gboolean
rpmostree_bwrap_bootstrap_if_in_mock (GError **error)
{
  const char *env_ps1 = getenv ("PS1");
  static const char *mock_mounted_paths[] = { "/proc", "/sys" };
  static const char *findmnt_argv[] = { "findmnt", "/", NULL };
  g_autofree char *pwd = NULL;
  int estatus;

  if (!(env_ps1 && strstr (env_ps1, "<mock-chroot>")))
    return TRUE;

  /* Okay, we detected we're inside mock.  Let's double check now
   * whether or not / is already a mount point.  The simplest way to
   * do this is to execute findmnt...maybe someday we'll link to libmount
   * but this is legacy.
   */
  if (!g_spawn_sync (NULL, (char**)findmnt_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                     NULL, NULL, &estatus, error))
    {
      g_prefix_error (error, "Executing findmnt: ");
      return FALSE;
    }
  /* Did findmnt say / is a mount point?  Okay, nothing to do here. */
  if (estatus == 0)
    return TRUE;

  pwd = getcwd (NULL, 0);

  g_print ("Detected mock chroot without / as mount, enabling workaround.\n");
  if (unshare (CLONE_NEWNS) < 0)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "unshare(CLONE_NEWNS)");
      return FALSE;
    }
  /* For reasons I don't fully understand, trying to bind mount / -> /
   * doesn't work.  We seem to hit a check in the kernel:
   *
   * static int do_change_type(struct path *path, int flag)
   * {
   *    ...
   * 	if (path->dentry != path->mnt->mnt_root)
   *		return -EINVAL;
   */
  if (mount ("/", "/mnt", NULL, MS_MGC_VAL | MS_BIND, NULL) != 0)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "mount(/ as bind)");
      return FALSE;
    }
  /* Now take the paths that mock mounted (that we need) and move them
   * underneath the new rootfs mount.
   */
  for (guint i = 0; i < G_N_ELEMENTS (mock_mounted_paths); i++)
    {
      const char *mockpath = mock_mounted_paths[i];
      g_autofree char *destpath = g_strconcat ("/mnt", mockpath, NULL);
      if (mount (mockpath, destpath, NULL, MS_MGC_VAL | MS_MOVE, NULL) != 0)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "mount(move)");
          return FALSE;
        }
    }
  if (chroot ("/mnt") < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }
  if (chdir (pwd) < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return TRUE;
}

/* Execute /bin/true inside a bwrap container on the host */
gboolean
rpmostree_bwrap_selftest (GError **error)
{
  glnx_fd_close int host_root_dfd = -1;
  g_autoptr(GPtrArray) bwrap_argv = NULL;

  if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &host_root_dfd, error))
    return FALSE;

  bwrap_argv = rpmostree_bwrap_base_argv_new_for_rootfs (host_root_dfd, error);
  if (!bwrap_argv)
    return FALSE;

  rpmostree_ptrarray_append_strdup (bwrap_argv,
                                    "--ro-bind", "usr", "/usr",
                                    NULL);
  g_ptr_array_add (bwrap_argv, g_strdup ("true"));
  g_ptr_array_add (bwrap_argv, NULL);
  if (!rpmostree_run_sync_fchdir_setup ((char**)bwrap_argv->pdata, G_SPAWN_SEARCH_PATH,
                                        host_root_dfd, error))
    {
      g_prefix_error (error, "bwrap test failed, see https://github.com/projectatomic/rpm-ostree/pull/429: ");
      return FALSE;
    }

  return TRUE;
}
