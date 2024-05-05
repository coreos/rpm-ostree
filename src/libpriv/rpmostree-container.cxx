/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2022 Red Hat, Inc.
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

#include <glib-unix.h>
#include <libdnf/libdnf.h>
#include <string.h>

#include "rpmostree-core.h"

#include "rpmostree-container.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-util.h"

#include <libglnx.h>

gboolean
rpmostree_container_rebuild (rpmostreecxx::Treefile &treefile, GCancellable *cancellable,
                             GError **error)
{
  CXX_TRY (treefile.validate_for_container (), error);

  g_autoptr (RpmOstreeContext) ctx = rpmostree_context_new_container ();
  rpmostree_context_set_treefile (ctx, treefile);

  glnx_autofd int rootfs_fd = -1;
  if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &rootfs_fd, error))
    return FALSE;

  // Forcibly turn this on for the container flow because it's the only sane
  // way for installing RPM packages that invoke useradd/groupadd to work.
  g_setenv ("RPMOSTREE_EXP_BRIDGE_SYSUSERS", "1", TRUE);

  // This is a duplicate of the bits in rpmostree-scripts.cxx which we need
  // for now because we aren't going through that code path today.
  g_setenv ("SYSTEMD_OFFLINE", "1", TRUE);

  // Ensure we have our wrappers for groupadd/systemctl set up
  CXX_TRY_VAR (fs_prep, rpmostreecxx::prepare_filesystem_script_prep (rootfs_fd), error);

  if (!rpmostree_context_setup (ctx, "/", "/", cancellable, error))
    return FALSE;

  if (!rpmostree_context_prepare (ctx, FALSE, cancellable, error))
    return FALSE;

  if (!rpmostree_context_download (ctx, cancellable, error))
    return FALSE;

  DnfContext *dnfctx = rpmostree_context_get_dnf (ctx);
  rpmostree_print_transaction (dnfctx);
  DnfTransaction *t = dnf_context_get_transaction (dnfctx);
  guint64 flags = dnf_transaction_get_flags (t);
  dnf_transaction_set_flags (t, flags | DNF_TRANSACTION_FLAG_ALLOW_DOWNGRADE);

  /* can't use cancellable here because it wants to re-set it on the state,
   * which will trigger an assertion; XXX: tweak libdnf */
  if (!dnf_context_run (dnfctx, NULL, error))
    return FALSE;

  CXX_TRY (fs_prep->undo (), error);

  CXX_TRY (rpmostreecxx::postprocess_cleanup_rpmdb (rootfs_fd), error);

  return TRUE;
}

namespace rpmostreecxx
{
void
container_rebuild (rust::Str treefile)
{
  auto tf = rpmostreecxx::treefile_new_from_string (treefile, true);
  g_autoptr (GError) local_error = NULL;
  if (!rpmostree_container_rebuild (*tf, NULL, &local_error))
    util::throw_gerror (local_error);
}
}
