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
#include "rpmostree-hif.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"

#include "libgsystem.h"

static gboolean opt_suid_fcaps = FALSE;
static gboolean opt_owner = FALSE;
static gboolean opt_to_ostree_repo = FALSE;

static GOptionEntry option_entries[] = {
  { "suid-fcaps", 0, 0, G_OPTION_ARG_NONE, &opt_suid_fcaps, "Enable setting suid/sgid and capabilities", NULL },
  { "owner", 0, 0, G_OPTION_ARG_NONE, &opt_owner, "Enable chown", NULL },
  { "to-ostree-repo", 0, 0, G_OPTION_ARG_NONE, &opt_to_ostree_repo, "Interpret TARGET as an OSTree repo", "REPO" },
  { NULL }
};

int
rpmostree_internals_builtin_unpack (int             argc,
                                    char          **argv,
                                    GCancellable   *cancellable,
                                    GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GOptionContext *context = g_option_context_new ("ROOT RPM");
  RpmOstreeUnpackerFlags flags = 0;
  glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
  const char *target;
  const char *rpmpath;
  glnx_fd_close int rootfs_fd = -1;
  glnx_unref_object OstreeRepo *ostree_repo = NULL;
  
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
      rpmostree_usage_error (context, "TARGET and RPM must be specified", error);
      goto out;
    }

  target = argv[1];
  rpmpath = argv[2];

  if (opt_to_ostree_repo)
    {
      g_autoptr(GFile) to_ostree_repo_file = g_file_new_for_path (target);

      ostree_repo = ostree_repo_new (to_ostree_repo_file);
      if (!ostree_repo_open (ostree_repo, cancellable, error))
        goto out;
    }
  else
    {
      if (!glnx_opendirat (AT_FDCWD, argv[1], TRUE, &rootfs_fd, error))
        goto out;
    }

  /* suid implies owner too...anything else is dangerous, as we might write
   * a setuid binary for the caller.
   */
  if (opt_owner || opt_suid_fcaps)
    flags |= RPMOSTREE_UNPACKER_FLAGS_OWNER;
  if (opt_suid_fcaps)
    flags |= RPMOSTREE_UNPACKER_FLAGS_SUID_FSCAPS;

  unpacker = rpmostree_unpacker_new_at (AT_FDCWD, rpmpath, flags, error);
  if (!unpacker)
    goto out;

  if (opt_to_ostree_repo)
    {
      const char *branch = rpmostree_unpacker_get_ostree_branch (unpacker);
      g_autofree char *checksum = NULL;

      if (!rpmostree_unpacker_unpack_to_ostree (unpacker, ostree_repo, NULL,
                                                &checksum, cancellable, error))
        goto out;

      g_print ("Imported %s to %s -> %s\n", rpmpath, branch, checksum);
    }
  else
    {
      if (!rpmostree_unpacker_unpack_to_dfd (unpacker, rootfs_fd, cancellable, error))
        goto out;
    }

  exit_status = EXIT_SUCCESS;
 out:
  return exit_status;
}
