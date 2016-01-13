/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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
#include <gio/gunixoutputstream.h>
#include <libhif.h>
#include <libhif/hif-utils.h>
#include <libhif/hif-package.h>
#include <rpm/rpmts.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-internals-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-hif.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"

#include "libgsystem.h"

static char *opt_ostree_repo;
static char *opt_yum_reposdir = "/etc/yum.repos.d";
static gboolean opt_suid_fcaps = FALSE;
static gboolean opt_owner = FALSE;
static char **opt_enable_yum_repos = NULL;

static GOptionEntry option_entries[] = {
  { "ostree-repo", 0, 0, G_OPTION_ARG_NONE, &opt_ostree_repo, "OSTree repo to use as cache", NULL },
  { "yum-reposdir", 0, 0, G_OPTION_ARG_STRING, &opt_yum_reposdir, "Path to yum repo configs (default: /etc/yum.repos.d)", NULL },
  { "enable-yum-repo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_yum_repos, "Enable yum repository", NULL },
  { "suid-fcaps", 0, 0, G_OPTION_ARG_NONE, &opt_suid_fcaps, "Enable setting suid/sgid and capabilities", NULL },
  { "owner", 0, 0, G_OPTION_ARG_NONE, &opt_owner, "Enable chown", NULL },
  { NULL }
};

static char *
hif_package_relpath (int rootfs_fd,
                     HyPackage package)
{
  return g_strconcat (".meta/repocache/", hy_package_get_reponame (package),
                      "/packages/", glnx_basename (hy_package_get_location (package)), NULL);
}

static gboolean
unpack_one_package (int           rootfs_fd,
                    HifContext   *hifctx,
                    HyPackage     pkg,
                    GCancellable *cancellable,
                    GError      **error)
{
  gboolean ret = FALSE;
  g_autofree char *package_relpath = NULL;
  g_autofree char *pkg_abspath = NULL;
  RpmOstreeUnpackerFlags flags = 0;
  glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
   
  package_relpath = hif_package_relpath (rootfs_fd, pkg);
  pkg_abspath = glnx_fdrel_abspath (rootfs_fd, package_relpath);

  /* suid implies owner too...anything else is dangerous, as we might write
   * a setuid binary for the caller.
   */
  if (opt_owner || opt_suid_fcaps)
    flags |= RPMOSTREE_UNPACKER_FLAGS_OWNER;
  if (opt_suid_fcaps)
    flags |= RPMOSTREE_UNPACKER_FLAGS_SUID_FSCAPS;
        
  unpacker = rpmostree_unpacker_new_at (rootfs_fd, package_relpath, flags, error);
  if (!unpacker)
    goto out;

  if (!rpmostree_unpacker_unpack_to_dfd (unpacker, rootfs_fd, cancellable, error))
    {
      g_autofree char *nevra = hy_package_get_nevra (pkg);
      g_prefix_error (error, "Unpacking %s: ", nevra);
      goto out;
    }
   
  if (TEMP_FAILURE_RETRY (unlinkat (rootfs_fd, package_relpath, 0)) < 0)
    {
      glnx_set_error_from_errno (error);
      g_prefix_error (error, "Deleting %s: ", package_relpath);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
unpack_packages_in_root (int          rootfs_fd,
                         HifContext  *hifctx,
                         GCancellable *cancellable,
                         GError      **error)
{
  gboolean ret = FALSE;
  rpmts ts = rpmtsCreate ();
  guint i, nElements;
  g_autoptr(GHashTable) nevra_to_pkg =
    g_hash_table_new_full (g_str_hash, g_str_equal, (GDestroyNotify)g_free, (GDestroyNotify)hy_package_free);
  HyPackage filesystem_package = NULL;   /* It's special... */

  /* Tell librpm about each one so it can tsort them.  What we really
   * want is to do this from the rpm-md metadata so that we can fully
   * parallelize download + unpack.
   */
  { g_autoptr(GPtrArray) package_list = NULL;

    package_list = hif_goal_get_packages (hif_context_get_goal (hifctx),
                                          HIF_PACKAGE_INFO_INSTALL,
                                          HIF_PACKAGE_INFO_REINSTALL,
                                          HIF_PACKAGE_INFO_DOWNGRADE,
                                          HIF_PACKAGE_INFO_UPDATE,
                                          -1);

    for (i = 0; i < package_list->len; i++)
      {
        HyPackage pkg = package_list->pdata[i];
        g_autofree char *package_relpath = hif_package_relpath (rootfs_fd, pkg);
        g_autofree char *pkg_abspath = glnx_fdrel_abspath (rootfs_fd, package_relpath);
        glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
        const gboolean allow_untrusted = TRUE;
        const gboolean is_update = FALSE;

        ret = hif_rpmts_add_install_filename (ts,
                                              pkg_abspath,
                                              allow_untrusted,
                                              is_update,
                                              error);
        if (!ret)
          goto out;

        g_hash_table_insert (nevra_to_pkg, hy_package_get_nevra (pkg), hy_package_link (pkg));

        if (strcmp (hy_package_get_name (pkg), "filesystem") == 0)
          filesystem_package = hy_package_link (pkg);
      }
  }

  rpmtsOrder (ts);

  /* Okay so what's going on in Fedora with incestuous relationship
   * between the `filesystem`, `setup`, `libgcc` RPMs is actively
   * ridiculous.  If we unpack libgcc first it writes to /lib64 which
   * is really /usr/lib64, then filesystem blows up since it wants to symlink
   * /lib64 -> /usr/lib64.
   *
   * Really `filesystem` should be first but it depends on `setup` for
   * stupid reasons which is hacked around in `%pretrans` which we
   * don't run.  Just forcibly unpack it first.
   */

  if (!unpack_one_package (rootfs_fd, hifctx, filesystem_package,
                           cancellable, error))
    goto out;

  nElements = (guint)rpmtsNElements (ts);
  for (i = 0; i < nElements; i++)
    {
      rpmte te = rpmtsElement (ts, i);
      const char *nevra = rpmteNEVRA (te);
      HyPackage pkg = g_hash_table_lookup (nevra_to_pkg, nevra);

      g_assert (pkg);

      if (pkg == filesystem_package)
        continue;

      if (!unpack_one_package (rootfs_fd, hifctx, pkg,
                               cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (ts)
    rpmtsFree (ts);
  return ret;
}

int
rpmostree_internals_builtin_mkroot (int             argc,
                                    char          **argv,
                                    GCancellable   *cancellable,
                                    GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("ROOT PKGNAME [PKGNAME...]");
  glnx_unref_object HifContext *hifctx = NULL;
  const char *rootpath;
  const char *const*pkgnames;
  glnx_fd_close int rootfs_fd = -1;
  
  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
                                       error))
    goto out;

  if (argc < 3)
    {
      rpmostree_usage_error (context, "ROOT and at least one PKGNAME must be specified", error);
      goto out;
    }

  rootpath = argv[1];
  pkgnames = (const char *const*)argv + 2;

  if (!glnx_opendirat (AT_FDCWD, argv[1], TRUE, &rootfs_fd, error))
    goto out;

  hifctx = _rpmostree_libhif_new_default ();

  { g_autofree char *cachepath = NULL;
    g_autofree char *solvpath = NULL;
    g_autofree char *lockpath = NULL;

    hif_context_set_install_root (hifctx, rootpath);
    cachepath = glnx_fdrel_abspath (rootfs_fd, ".meta/repocache");
    hif_context_set_cache_dir (hifctx, cachepath);
    hif_context_set_cache_age (hifctx, G_MAXUINT);
    solvpath = glnx_fdrel_abspath (rootfs_fd, ".meta/solv");
    hif_context_set_solv_dir (hifctx, solvpath);
    lockpath = glnx_fdrel_abspath (rootfs_fd, ".meta/lock");
    hif_context_set_lock_dir (hifctx, lockpath);
    hif_context_set_repo_dir (hifctx, opt_yum_reposdir);
  }

  if (!_rpmostree_libhif_setup (hifctx, cancellable, error))
    goto out;
  _rpmostree_libhif_repos_disable_all (hifctx);

  { char **strviter = opt_enable_yum_repos;
    for (; strviter && *strviter; strviter++)
      {
        const char *reponame = *strviter;
        if (!_rpmostree_libhif_repos_enable_by_name (hifctx, reponame, error))
          goto out;
      }
  }

  /* --- Downloading metadata --- */
  if (!_rpmostree_libhif_console_download_metadata (hifctx, cancellable, error))
    goto out;

  { const char *const*strviter = pkgnames;
    for (; strviter && *strviter; strviter++)
      {
        const char *pkgname = *strviter;
        if (!hif_context_install (hifctx, pkgname, error))
          goto out;
      }
  }

  /* --- Resolving dependencies --- */
  if (!_rpmostree_libhif_console_depsolve (hifctx, cancellable, error))
    goto out;

  rpmostree_print_transaction (hifctx);

  /* --- Downloading packages --- */
  if (!_rpmostree_libhif_console_download_content (hifctx, cancellable, error))
    goto out;

  if (!unpack_packages_in_root (rootfs_fd, hifctx, cancellable, error))
    goto out;

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
