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

gboolean
rpmostree_run_bwrap_sync (char **argv_array, int rootfs_fd, GError **error)
{
  int estatus;
  const char *current_lang = getenv ("LANG");

  if (!current_lang)
    current_lang = "C";

  {
    const char *lang_var = glnx_strjoina ("LANG=", current_lang);
    /* This is similar to what systemd does, except:
     *  - We drop /usr/local, since scripts shouldn't see it.
     *  - We pull in the current process' LANG, since that's what people
     *    have historically expected from RPM scripts.
     */
    const char *bwrap_env[] = {"PATH=/usr/sbin:/usr/bin",
                               lang_var,
                               NULL};

    if (!g_spawn_sync (NULL, argv_array, (char**) bwrap_env, G_SPAWN_SEARCH_PATH,
                       child_setup_fchdir, GINT_TO_POINTER (rootfs_fd),
                       NULL, NULL, &estatus, error))
      return FALSE;
    if (!g_spawn_check_exit_status (estatus, error))
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
  if (!rpmostree_run_bwrap_sync ((char**)bwrap_argv->pdata, host_root_dfd, error))
    {
      g_prefix_error (error, "bwrap test failed, see <https://github.com/projectatomic/rpm-ostree/pull/429>: ");
      return FALSE;
    }

  return TRUE;
}
