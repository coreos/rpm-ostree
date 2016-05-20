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
#include <rpm/rpmts.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-internals-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"
#include "rpmostree-postprocess.h"

#include "libgsystem.h"

static gboolean opt_selinux = FALSE;
static gboolean opt_ostree_convention = FALSE;

static GOptionEntry option_entries[] = {
  { "selinux", 0, 0, G_OPTION_ARG_NONE, &opt_selinux,
      "Enable setting SELinux labels", NULL },
  { "ostree-convention", 0, 0, G_OPTION_ARG_NONE, &opt_ostree_convention,
      "Change file paths following ostree conventions", NULL },
  { NULL }
};

int
rpmostree_internals_builtin_unpack (int             argc,
                                    char          **argv,
                                    GCancellable   *cancellable,
                                    GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("REPO RPM");
  RpmOstreeUnpackerFlags flags = 0;
  glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
  const char *target;
  const char *rpmpath;
  glnx_fd_close int rootfs_fd = -1;
  glnx_unref_object OstreeRepo *ostree_repo = NULL;
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;

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
      rpmostree_usage_error (context, "REPO and RPM must be specified", error);
      goto out;
    }

  target = argv[1];
  rpmpath = argv[2];

  {
    g_autoptr(GFile) ostree_repo_file = g_file_new_for_path (target);

    ostree_repo = ostree_repo_new (ostree_repo_file);
    if (!ostree_repo_open (ostree_repo, cancellable, error))
      goto out;
  }

  if (opt_ostree_convention)
    flags |= RPMOSTREE_UNPACKER_FLAGS_OSTREE_CONVENTION;

  unpacker = rpmostree_unpacker_new_at (AT_FDCWD, rpmpath, flags, error);
  if (!unpacker)
    goto out;

  /* just use current policy */
  if (opt_selinux)
    if (!rpmostree_prepare_rootfs_get_sepolicy (AT_FDCWD, "/", &sepolicy,
                                                cancellable, error))
      goto out;

  {
    const char *branch = rpmostree_unpacker_get_ostree_branch (unpacker);
    g_autofree char *checksum = NULL;

    if (!rpmostree_unpacker_unpack_to_ostree (unpacker, ostree_repo, sepolicy,
                                              &checksum, cancellable, error))
      goto out;

    g_print ("Imported %s to %s -> %s\n", rpmpath, branch, checksum);
  }

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
