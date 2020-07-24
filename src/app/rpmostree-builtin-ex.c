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

#include "rpmostree-ex-builtins.h"

static RpmOstreeCommand ex_subcommands[] = {
  { "livefs", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    "Apply pending deployment changes to booted deployment",
    rpmostree_ex_builtin_livefs },
  { "container", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Assemble local unprivileged containers", rpmostree_builtin_container },
#ifdef BUILDOPT_ROJIG
  { "commit2rojig", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Convert an OSTree commit into an rpm-ostree rojig", rpmostree_ex_builtin_commit2rojig },
  { "rojig2commit", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Convert an rpm-ostree rojig into an OSTree commit", rpmostree_ex_builtin_rojig2commit },
#endif
  { "history", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Inspect rpm-ostree history of the system", rpmostree_ex_builtin_history },
  { "initramfs-etc", RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT,
    "Track initramfs configuration files", rpmostree_ex_builtin_initramfs_etc },
  { NULL, 0, NULL, NULL }
};

/*
static GOptionEntry global_entries[] = {
  { NULL }
};
*/

gboolean
rpmostree_builtin_ex (int argc, char **argv,
                      RpmOstreeCommandInvocation *invocation,
                      GCancellable *cancellable, GError **error)
{
  return rpmostree_handle_subcommand (argc, argv, ex_subcommands,
                                      invocation, cancellable, error);
}

