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
