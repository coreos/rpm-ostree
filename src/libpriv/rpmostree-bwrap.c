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
  GLnxTmpDir rofiles_mnt;

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

  if (bwrap->rofiles_mnt.initialized)
    {
      g_autoptr(GError) tmp_error = NULL;
      const char *fusermount_argv[] = { "fusermount", "-u", bwrap->rofiles_mnt.path, NULL};
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
  (void)glnx_tmpdir_delete (&bwrap->rofiles_mnt, NULL, NULL);

  g_ptr_array_unref (bwrap->argv);
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
  const char *rofiles_argv[] = { "rofiles-fuse", "./usr", NULL, NULL};

  if (!glnx_mkdtemp ("rpmostree-rofiles-fuse.XXXXXX", 0700, &bwrap->rofiles_mnt, error))
    return FALSE;

  const char *rofiles_mntpath = bwrap->rofiles_mnt.path;
  rofiles_argv[2] = rofiles_mntpath;

  int estatus;
  if (!g_spawn_sync (NULL, (char**)rofiles_argv, NULL, G_SPAWN_SEARCH_PATH,
                     child_setup_fchdir, GINT_TO_POINTER (bwrap->rootfs_fd),
                     NULL, NULL, &estatus, error))
    return FALSE;
  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;

  rpmostree_bwrap_append_bwrap_argv (bwrap, "--bind", rofiles_mntpath, "/usr", NULL);

  /* also mount /etc from the rofiles mount to allow RPM scripts to change defaults, while
   * still being protected; note we use bind to ensure symlinks work, see:
   * https://github.com/projectatomic/rpm-ostree/pull/640 */
  const char *rofiles_etc_mntpath = glnx_strjoina (rofiles_mntpath, "/etc");
  rpmostree_bwrap_append_bwrap_argv (bwrap, "--bind", rofiles_etc_mntpath, "/etc", NULL);

  return TRUE;
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

  /* Capabilities; this is a subset of the Docker (1.13 at least) default.
   * Specifically we strip out in addition to that:
   *
   * "cap_net_raw" (no use for this in %post, and major source of security vulnerabilities)
   * "cap_mknod" (%post should not be making devices, it wouldn't be persistent anyways)
   * "cap_audit_write" (we shouldn't be auditing anything from here)
   * "cap_net_bind_service" (nothing should be doing IP networking at all)
   *
   * But crucially we're dropping a lot of other capabilities like
   * "cap_sys_admin", "cap_sys_module", etc that Docker also drops by default.
   * We don't want RPM scripts to be doing any of that. Instead, do it from
   * systemd unit files.
   *
   * Also this way we drop out any new capabilities that appear.
   */
  if (getuid () == 0)
    rpmostree_bwrap_append_bwrap_argv (ret, "--cap-drop", "ALL",
                                       "--cap-add", "cap_chown",
                                       "--cap-add", "cap_dac_override",
                                       "--cap-add", "cap_fowner",
                                       "--cap-add", "cap_fsetid",
                                       "--cap-add", "cap_kill",
                                       "--cap-add", "cap_setgid",
                                       "--cap-add", "cap_setuid",
                                       "--cap-add", "cap_setpcap",
                                       "--cap-add", "cap_sys_chroot",
                                       "--cap-add", "cap_setfcap",
                                       NULL);

  for (guint i = 0; i < G_N_ELEMENTS (usr_links); i++)
    {
      const char *subdir = usr_links[i];
      struct stat stbuf;
      g_autofree char *srcpath = NULL;
      g_autofree char *destpath = NULL;

      if (!glnx_fstatat_allow_noent (rootfs_fd, subdir, &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == ENOENT || !S_ISLNK (stbuf.st_mode))
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

/* Execute @bwrap - must have been configured. After executing this method, the
 * @bwrap instance cannot be run again.
 */
gboolean
rpmostree_bwrap_run (RpmOstreeBwrap *bwrap,
                     GCancellable   *cancellable,
                     GError        **error)
{
  g_assert (!bwrap->executed);
  bwrap->executed = TRUE;

  const char *current_lang = getenv ("LANG");
  if (!current_lang)
    current_lang = "C";

  const char *lang_var = glnx_strjoina ("LANG=", current_lang);
  /* This is similar to what systemd does, except:
   *  - We drop /usr/local, since scripts shouldn't see it.
   *  - We pull in the current process' LANG, since that's what people
   *    have historically expected from RPM scripts.
   */
  const char *bwrap_env[] = {"PATH=/usr/sbin:/usr/bin", lang_var, NULL};

  /* Set up our error message */
  const char *errmsg = glnx_strjoina ("Executing bwrap(", bwrap->child_argv0, ")");
  GLNX_AUTO_PREFIX_ERROR (errmsg, error);

  /* Add the final NULL */
  g_ptr_array_add (bwrap->argv, NULL);

  g_autoptr(GSubprocessLauncher) launcher =
    g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_set_environ (launcher, (char**)bwrap_env);
  g_subprocess_launcher_set_child_setup (launcher, bwrap_child_setup, bwrap, NULL);
  g_autoptr(GSubprocess) subproc =
    g_subprocess_launcher_spawnv (launcher, (const char *const*)bwrap->argv->pdata,
                                  error);
  if (!subproc)
    return FALSE;
  if (!g_subprocess_wait (subproc, cancellable, error))
    {
      /* Now, it's possible @cancellable has been set, which means the process
       * hasn't terminated yet. AFAIK that should be the only cause for the
       * process not having exited now, but we just kill the process regardless
       * on error here.  The GSubprocess code ignores the request if we've
       * already reaped it.
       *
       * Right now we run bwrap --die-with-parent, but until we do the whole txn
       * as a subprocess, the script would leak until rpm-ostreed exited.
       */
      g_subprocess_force_exit (subproc);
      return FALSE;
    }
  int estatus = g_subprocess_get_exit_status (subproc);
  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;

  return TRUE;
}

/* Execute /bin/true inside a bwrap container on the host */
gboolean
rpmostree_bwrap_selftest (GError **error)
{
  glnx_autofd int host_root_dfd = -1;
  g_autoptr(RpmOstreeBwrap) bwrap = NULL;

  if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &host_root_dfd, error))
    return FALSE;

  bwrap = rpmostree_bwrap_new (host_root_dfd, RPMOSTREE_BWRAP_IMMUTABLE, error, NULL);
  if (!bwrap)
    return FALSE;

  rpmostree_bwrap_append_child_argv (bwrap, "true", NULL);

  if (!rpmostree_bwrap_run (bwrap, NULL, error))
    {
      g_prefix_error (error, "bwrap test failed, see <https://github.com/projectatomic/rpm-ostree/pull/429>: ");
      return FALSE;
    }

  return TRUE;
}
