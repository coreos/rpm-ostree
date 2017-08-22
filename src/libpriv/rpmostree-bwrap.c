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
#include <stdio.h>
#include <systemd/sd-journal.h>

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

struct RpmOstreeBwrap {
  guint refcount;

  gboolean executed;

  int rootfs_fd;

  GPtrArray *argv;
  const char *child_argv0;
  char *rofiles_mnt;

  GSpawnChildSetupFunc child_setup_func;
  gpointer child_setup_data;
};

RpmOstreeBwrap *
rpmostree_bwrap_ref (RpmOstreeBwrap *bwrap)
{
  bwrap->refcount++;
  return bwrap;
}

void
rpmostree_bwrap_unref (RpmOstreeBwrap *bwrap)
{
  bwrap->refcount--;
  if (bwrap->refcount > 0)
    return;

  if (bwrap->rofiles_mnt)
    {
      g_autoptr(GError) tmp_error = NULL;
      const char *fusermount_argv[] = { "fusermount", "-u", bwrap->rofiles_mnt, NULL};
      int estatus;

      if (!g_spawn_sync (NULL, (char**)fusermount_argv, NULL, G_SPAWN_SEARCH_PATH,
                         NULL, NULL, NULL, NULL, &estatus, &tmp_error))
        {
          g_prefix_error (&tmp_error, "Executing fusermount: ");
          goto out;
        }
      if (!g_spawn_check_exit_status (estatus, &tmp_error))
        {
          g_prefix_error (&tmp_error, "Executing fusermount: ");
          goto out;
        }

      (void) unlinkat (AT_FDCWD, bwrap->rofiles_mnt, AT_REMOVEDIR);
    out:
      /* We don't want a failure to unmount to be fatal, so all we do here
       * is log.  Though in practice what we *really* want is for the
       * fusermount to be in the bwrap namespace, and hence tied by the
       * kernel to the lifecycle of the container.  This would require
       * special casing for somehow doing FUSE mounts in bwrap.  Which
       * would be hard because NO_NEW_PRIVS turns off the setuid bits for
       * fuse.
       */
      if (tmp_error)
        sd_journal_print (LOG_WARNING, "%s", tmp_error->message);
    }

  g_ptr_array_unref (bwrap->argv);
  g_free (bwrap->rofiles_mnt);
  g_free (bwrap);
}

void
rpmostree_bwrap_append_bwrap_argv (RpmOstreeBwrap *bwrap, ...)
{
  va_list args;
  char *arg;

  g_assert (!bwrap->executed);

  va_start (args, bwrap);
  while ((arg = va_arg (args, char *)))
    g_ptr_array_add (bwrap->argv, g_strdup (arg));
  va_end (args);
}

void
rpmostree_bwrap_append_child_argv (RpmOstreeBwrap *bwrap, ...)
{
  va_list args;
  char *arg;

  g_assert (!bwrap->executed);

  va_start (args, bwrap);
  while ((arg = va_arg (args, char *)))
    {
      char *v = g_strdup (arg);
      g_ptr_array_add (bwrap->argv, v);
      /* Stash argv0 for error messages */
      if (!bwrap->child_argv0)
        bwrap->child_argv0 = v;
    }
  va_end (args);
}

static void
child_setup_fchdir (gpointer user_data)
{
  int fd = GPOINTER_TO_INT (user_data);
  if (fchdir (fd) < 0)
    err (1, "fchdir");
}

static gboolean
setup_rofiles_usr (RpmOstreeBwrap *bwrap,
                   GError **error)
{
  gboolean ret = FALSE;
  int estatus;
  const char *rofiles_argv[] = { "rofiles-fuse", "./usr", NULL, NULL};
  gboolean mntpoint_created = FALSE;

  bwrap->rofiles_mnt = g_strdup ("/tmp/rofiles-fuse.XXXXXX");
  rofiles_argv[2] = bwrap->rofiles_mnt;

  if (!glnx_mkdtempat (AT_FDCWD, bwrap->rofiles_mnt, 0700, error))
    goto out;
  mntpoint_created = TRUE;

  if (!g_spawn_sync (NULL, (char**)rofiles_argv, NULL, G_SPAWN_SEARCH_PATH,
                     child_setup_fchdir, GINT_TO_POINTER (bwrap->rootfs_fd),
                     NULL, NULL, &estatus, error))
    goto out;
  if (!g_spawn_check_exit_status (estatus, error))
    goto out;

  rpmostree_bwrap_append_bwrap_argv (bwrap, "--bind", bwrap->rofiles_mnt, "/usr", NULL);

  ret = TRUE;
 out:
  if (!ret && mntpoint_created)
    (void) unlinkat (AT_FDCWD, bwrap->rofiles_mnt, AT_REMOVEDIR);
  return ret;
}

/* nspawn by default doesn't give us CAP_NET_ADMIN; see
 * https://pagure.io/releng/issue/6602#comment-71214
 * https://pagure.io/koji/pull-request/344#comment-21060
 *
 * Theoretically we should do capable(CAP_NET_ADMIN)
 * but that's a lot of ugly code, and the only known
 * place we hit this right now is nspawn.  Plus
 * we want to use userns down the line anyways where
 * we'll regain CAP_NET_ADMIN.
 */
static gboolean
running_in_nspawn (void)
{
  return g_strcmp0 (getenv ("container"), "systemd-nspawn") == 0;
}

RpmOstreeBwrap *
rpmostree_bwrap_new (int rootfs_fd,
                     RpmOstreeBwrapMutability mutable,
                     GError **error,
                     ...)
{
  va_list args;
  g_autoptr(RpmOstreeBwrap) ret = g_new0 (RpmOstreeBwrap, 1);
  static const char *usr_links[] = {"lib", "lib32", "lib64", "bin", "sbin"};

  ret->refcount = 1;
  ret->rootfs_fd = rootfs_fd;
  ret->argv = g_ptr_array_new_with_free_func (g_free);

  /* ⚠⚠⚠ If you change this, also update scripts/bwrap-script-shell.sh ⚠⚠⚠ */
  rpmostree_bwrap_append_bwrap_argv (ret,
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
                                     "--die-with-parent", /* Since 0.1.8 */
                                     /* Here we do all namespaces except the user one.
                                      * Down the line we want to do a userns too I think,
                                      * but it may need some mapping work.
                                      */
                                     "--unshare-pid",
                                     "--unshare-uts",
                                     "--unshare-ipc",
                                     "--unshare-cgroup-try",
                                     NULL);

  if (!running_in_nspawn ())
    rpmostree_bwrap_append_bwrap_argv (ret, "--unshare-net", NULL);

  for (guint i = 0; i < G_N_ELEMENTS (usr_links); i++)
    {
      const char *subdir = usr_links[i];
      struct stat stbuf;
      g_autofree char *srcpath = NULL;
      g_autofree char *destpath = NULL;

      if (fstatat (rootfs_fd, subdir, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
        {
          if (errno != ENOENT)
            return glnx_null_throw_errno_prefix (error, "fstatat");
          continue;
        }
      else if (!S_ISLNK (stbuf.st_mode))
        continue;

      srcpath = g_strconcat ("usr/", subdir, NULL);
      destpath = g_strconcat ("/", subdir, NULL);
      rpmostree_bwrap_append_bwrap_argv (ret, "--symlink", srcpath, destpath, NULL);
    }

  switch (mutable)
    {
    case RPMOSTREE_BWRAP_IMMUTABLE:
      rpmostree_bwrap_append_bwrap_argv (ret, "--ro-bind", "usr", "/usr", NULL);
      break;
    case RPMOSTREE_BWRAP_MUTATE_ROFILES:
      if (!setup_rofiles_usr (ret, error))
        return NULL;
      break;
    case RPMOSTREE_BWRAP_MUTATE_FREELY:
      rpmostree_bwrap_append_bwrap_argv (ret, "--bind", "usr", "/usr", NULL);
      break;
    }

  { const char *arg;
    va_start (args, error);
    while ((arg = va_arg (args, char *)))
      g_ptr_array_add (ret->argv, g_strdup (arg));
    va_end (args);
  }

  return g_steal_pointer (&ret);
}

static void
bwrap_child_setup (gpointer data)
{
  RpmOstreeBwrap *bwrap = data;

  if (fchdir (bwrap->rootfs_fd) < 0)
    err (1, "fchdir");

  if (bwrap->child_setup_func)
    bwrap->child_setup_func (bwrap->child_setup_data);
}

void
rpmostree_bwrap_set_child_setup (RpmOstreeBwrap *bwrap,
                                 GSpawnChildSetupFunc func,
                                 gpointer             data)
{
  g_assert (!bwrap->executed);
  bwrap->child_setup_func = func;
  bwrap->child_setup_data = data;
}

gboolean
rpmostree_bwrap_run (RpmOstreeBwrap *bwrap,
                     GError **error)
{
  int estatus;
  const char *current_lang = getenv ("LANG");

  g_assert (!bwrap->executed);
  bwrap->executed = TRUE;

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

    /* Add the final NULL */
    g_ptr_array_add (bwrap->argv, NULL);

    const char *errmsg = glnx_strjoina ("Executing bwrap(", bwrap->child_argv0, ")");
    GLNX_AUTO_PREFIX_ERROR (errmsg, error);

    if (!g_spawn_sync (NULL, (char**)bwrap->argv->pdata, (char**) bwrap_env, G_SPAWN_SEARCH_PATH,
                       bwrap_child_setup, bwrap,
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
  g_autoptr(RpmOstreeBwrap) bwrap = NULL;

  if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &host_root_dfd, error))
    return FALSE;

  bwrap = rpmostree_bwrap_new (host_root_dfd, RPMOSTREE_BWRAP_IMMUTABLE, error, NULL);
  if (!bwrap)
    return FALSE;

  rpmostree_bwrap_append_child_argv (bwrap, "true", NULL);

  if (!rpmostree_bwrap_run (bwrap, error))
    {
      g_prefix_error (error, "bwrap test failed, see <https://github.com/projectatomic/rpm-ostree/pull/429>: ");
      return FALSE;
    }

  return TRUE;
}
