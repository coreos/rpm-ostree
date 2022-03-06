/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2020 Red Hat, Inc.
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
#include <gio/gio.h>
#include <gio/gunixfdmessage.h>
#include <gio/gunixsocketaddress.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include "rpmostree-builtins.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-cxxrs.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree.h"
#include "rpmostree-core.h"
#include "src/lib/rpmostree-shlib-ipc-private.h"

#include <libglnx.h>

static gboolean
send_memfd_result (GSocket *ipc_sock, int ret_memfd, GError **error)
{
  int fdarray[] = {ret_memfd, -1 };
  g_autoptr(GUnixFDList) list = g_unix_fd_list_new_from_array (fdarray, 1);
  g_autoptr(GUnixFDMessage) message = G_UNIX_FD_MESSAGE (g_unix_fd_message_new_with_fd_list (list));

  GOutputVector ov;
  char buffer[1];
  buffer[0] = 0xFF;
  ov.buffer = buffer;
  ov.size = G_N_ELEMENTS (buffer);
  gssize r = g_socket_send_message (ipc_sock, NULL, &ov, 1,
                                    (GSocketControlMessage **) &message,
                                    1, 0, NULL, error);
  if (r < 0)
    return FALSE;
  g_assert_cmpint (r, ==, 1);

  return TRUE;
}

static GVariant *
impl_packagelist_from_commit (OstreeRepo *repo, const char *commit, GError **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(RpmOstreeRefSack) rsack =
    rpmostree_get_refsack_for_commit (repo, commit, NULL, &local_error);
  if (!rsack)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return g_variant_new_maybe ((GVariantType*) RPMOSTREE_SHLIB_IPC_PKGLIST, NULL);
      g_propagate_error (error, util::move_nullify (local_error));
      return NULL;
    }

  g_autoptr(GVariant) pkgs = rpmostree_variant_pkgs_from_sack (rsack);
  // It can happen that we successfully query zero packages.  For example,
  // when rpm-ostree on RHEL8 (assuming bdb database) is trying to parse an
  // ostree commit generated from fedora (sqlite rpmdb), librpm will just give us
  // nothing.  Eventually perhaps we may need to fall back to actually running
  // the target commit as a container just to get this data for cases like that.
  if (g_variant_n_children (pkgs) == 0)
    return g_variant_new_maybe ((GVariantType*) RPMOSTREE_SHLIB_IPC_PKGLIST, NULL);
  return g_variant_ref_sink (g_variant_new_maybe ((GVariantType*) RPMOSTREE_SHLIB_IPC_PKGLIST, pkgs));
}

gboolean
rpmostree_builtin_shlib_backend (int             argc,
                                 char          **argv,
                                 RpmOstreeCommandInvocation *invocation,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  if (argc < 2)
    return glnx_throw (error, "missing required subcommand");

  const char *arg = argv[1];

  g_autoptr(GSocket) ipc_sock = g_socket_new_from_fd (RPMOSTREE_SHLIB_IPC_FD, error);
  if (!ipc_sock)
    return FALSE;

  g_autoptr(GVariant) ret = NULL;

  if (g_str_equal (arg, "get-basearch"))
    {
      g_autoptr(DnfContext) ctx = dnf_context_new ();
      ret = g_variant_new_string (dnf_context_get_base_arch (ctx));
    }
  else if (g_str_equal (arg, "varsubst-basearch"))
    {
      const char *src = argv[2];
      g_autoptr(DnfContext) ctx = dnf_context_new ();
      auto varsubsts = rpmostree_dnfcontext_get_varsubsts (ctx);
      auto rets = CXX_TRY_VAL(varsubstitute (src, *varsubsts), error);
      ret = g_variant_new_string (rets.c_str());
    }
   else if (g_str_equal (arg, "packagelist-from-commit"))
    {
      OstreeRepo *repo = ostree_repo_open_at (AT_FDCWD, ".", NULL, error);
      if (!repo)
        return FALSE;
      const char *commit = argv[2];
      ret = impl_packagelist_from_commit (repo, commit, error);
      if (!ret)
        return FALSE;
    }
  else
    return glnx_throw (error, "unknown shlib-backend %s", arg);

  rust::Slice<const uint8_t> dataslice{(guint8*)g_variant_get_data (ret), g_variant_get_size (ret)};
  glnx_fd_close int ret_memfd = CXX_TRY_VAL (sealed_memfd("rpm-ostree-shlib-backend", dataslice), error);
  return send_memfd_result (ipc_sock, glnx_steal_fd (&ret_memfd), error);
}
