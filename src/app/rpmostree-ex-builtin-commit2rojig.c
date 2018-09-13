/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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
#include <libdnf/libdnf.h>
#include <sys/mount.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-ex-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-rojig-core.h"
#include "rpmostree-rojig-build.h"
#include "rpmostree-rust.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"

#include "libglnx.h"

static char *opt_repo;
static char *opt_pkgcache_repo;
static gboolean opt_only_contentdir;

static GOptionEntry commit2rojig_option_entries[] = {
  { "repo", 0, 0, G_OPTION_ARG_STRING, &opt_repo, "OSTree repo", "REPO" },
  { "pkgcache-repo", 0, 0, G_OPTION_ARG_STRING, &opt_pkgcache_repo, "Pkgcache OSTree repo", "REPO" },
  { "only-contentdir", 0, 0, G_OPTION_ARG_NONE, &opt_only_contentdir, "Do not generate RPM, only output content directory", NULL },
  { NULL }
};

gboolean
rpmostree_ex_builtin_commit2rojig (int             argc,
                                   char          **argv,
                                   RpmOstreeCommandInvocation *invocation,
                                   GCancellable   *cancellable,
                                   GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("REV TREEFILE OUTPUTDIR");
  if (!rpmostree_option_context_parse (context,
                                       commit2rojig_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc != 4)
    {
      rpmostree_usage_error (context, "REV OIRPM-SPEC OUTPUTDIR are required", error);
      return FALSE;
    }

  if (!(opt_repo && opt_pkgcache_repo))
    {
      rpmostree_usage_error (context, "--repo and --pkgcache-repo must be specified", error);
      return FALSE;
    }

  const char *rev = argv[1];
  const char *treefile = argv[2];
  const char *outputdir = argv[3];
  if (!g_str_has_prefix (outputdir, "/"))
    return glnx_throw (error, "outputdir must be absolute");

  g_autoptr(OstreeRepo) repo = ostree_repo_open_at (AT_FDCWD, opt_repo, cancellable, error);
  if (!repo)
    return FALSE;
  g_autoptr(OstreeRepo) pkgcache_repo = ostree_repo_open_at (AT_FDCWD, opt_pkgcache_repo, cancellable, error);
  if (!pkgcache_repo)
    return FALSE;

  g_auto(GLnxTmpDir) tmpd = { 0, };
  if (!glnx_mkdtemp ("rpmostree-commit2rojig-XXXXXX", 0700, &tmpd, error))
    return FALSE;

  g_autoptr(RORTreefile) treefile_rs = ror_treefile_new (treefile, NULL, tmpd.fd, error);
  const char *rojig_spec_path = ror_treefile_get_rojig_spec_path (treefile_rs);
  if (!rpmostree_commit2rojig (repo, pkgcache_repo, rev, tmpd.fd, rojig_spec_path, outputdir,
                               cancellable, error))
    return FALSE;

  return TRUE;
}
