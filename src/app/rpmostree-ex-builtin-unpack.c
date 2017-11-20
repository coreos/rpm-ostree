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
#include <libdnf/libdnf.h>
#include <rpm/rpmts.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-ex-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-importer.h"
#include "rpmostree-postprocess.h"

#include "libglnx.h"

static gboolean opt_selinux = FALSE;

static GOptionEntry option_entries[] = {
  { "selinux", 0, 0, G_OPTION_ARG_NONE, &opt_selinux,
      "Enable setting SELinux labels", NULL },
  { NULL }
};

int
rpmostree_ex_builtin_unpack (int             argc,
                             char          **argv,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable   *cancellable,
                             GError        **error)
{
  int exit_status = EXIT_FAILURE;
  g_autoptr(GOptionContext) context = g_option_context_new ("REPO RPM");
  RpmOstreeImporterFlags flags = 0;
  glnx_unref_object RpmOstreeImporter *unpacker = NULL;
  const char *target;
  const char *rpmpath;
  glnx_unref_object OstreeRepo *ostree_repo = NULL;
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL,
                                       error))
    goto out;

  if (argc < 3)
    {
      rpmostree_usage_error (context, "REPO and RPM must be specified", error);
      goto out;
    }

  target = argv[1];
  rpmpath = argv[2];

  ostree_repo = ostree_repo_open_at (AT_FDCWD, target, cancellable, error);
  if (!ostree_repo)
    goto out;

  unpacker = rpmostree_importer_new_at (AT_FDCWD, rpmpath, NULL, flags, error);
  if (!unpacker)
    goto out;

  /* just use current policy */
  if (opt_selinux)
    {
      glnx_autofd int rootfs_dfd = -1;
      if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &rootfs_dfd, error))
        goto out;
      sepolicy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
      if (!sepolicy)
        goto out;
    }

  {
    const char *branch = rpmostree_importer_get_ostree_branch (unpacker);
    g_autofree char *checksum = NULL;

    if (!rpmostree_importer_run (unpacker, ostree_repo, sepolicy,
                                 &checksum, cancellable, error))
      goto out;

    g_print ("Imported %s to %s -> %s\n", rpmpath, branch, checksum);
  }

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
