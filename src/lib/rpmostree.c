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

#include "libglnx.h"
#include "string.h"
#include <gio/gio.h>
#include <gio/gunixfdmessage.h>
#include <gio/gunixsocketaddress.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "rpmostree-shlib-ipc-private.h"
#include "rpmostree.h"

/**
 * SECTION:librpmostree
 * @title: Global high level APIs
 * @short_description: APIs for accessing global state
 *
 * These APIs access generic global state.
 */

GVariant *
_rpmostree_shlib_ipc_send (const char *variant_type, char **args, const char *wd, GError **error)
{
  g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  int pair[2];
  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
    return (GVariant *)glnx_null_throw_errno_prefix (error, "couldn't create socket pair");
  glnx_fd_close int my_sock_fd = glnx_steal_fd (&pair[0]);
  g_subprocess_launcher_take_fd (launcher, pair[1], RPMOSTREE_SHLIB_IPC_FD);

  if (wd != NULL)
    g_subprocess_launcher_set_cwd (launcher, wd);

  g_autoptr (GSocket) my_sock = g_socket_new_from_fd (my_sock_fd, error);
  if (!my_sock)
    return NULL;
  my_sock_fd = -1;
  (void)my_sock_fd; /* Ownership was transferred */
  g_autoptr (GPtrArray) full_args = g_ptr_array_new ();
  g_ptr_array_add (full_args, "rpm-ostree");
  g_ptr_array_add (full_args, "shlib-backend");
  for (char **it = args; it && *it; it++)
    g_ptr_array_add (full_args, *it);
  g_ptr_array_add (full_args, NULL);
  g_autoptr (GSubprocess) proc
      = g_subprocess_launcher_spawnv (launcher, (const char *const *)full_args->pdata, error);
  if (!proc)
    return NULL;

  g_autofree char *stderr = NULL;
  if (!g_subprocess_communicate_utf8 (proc, NULL, NULL, NULL, &stderr, error))
    return NULL;

  if (!g_subprocess_get_successful (proc))
    return glnx_null_throw (error, "Failed to invoke rpm-ostree shlib-backend: %s", stderr);
  int flags = 0;
  int nm = 0;
  GInputVector iv;
  GUnixFDMessage **mv;
  guint8 buffer[1024];
  iv.buffer = buffer;
  iv.size = 1;
  gssize r = g_socket_receive_message (my_sock, NULL, &iv, 1, (GSocketControlMessage ***)&mv, &nm,
                                       &flags, NULL, error);
  if (r < 0)
    return NULL;
  g_assert_cmpint (r, ==, 1);
  g_assert_cmphex (buffer[0], ==, 0xFF);

  if (nm != 1)
    return glnx_null_throw (error, "Got %d control messages, expected 1", nm);
  GUnixFDMessage *message = mv[0];
  g_assert (G_IS_UNIX_FD_MESSAGE (message));
  GUnixFDList *fdlist = g_unix_fd_message_get_fd_list (message);
  const int nfds = g_unix_fd_list_get_length (fdlist);
  if (nfds != 1)
    return glnx_null_throw (error, "Got %d fds, expected 1", nfds);
  const int *fds = g_unix_fd_list_peek_fds (fdlist, NULL);
  const int result_memfd = fds[0];
  g_assert_cmpint (result_memfd, !=, -1);
  g_autoptr (GMappedFile) retmap = g_mapped_file_new_from_fd (result_memfd, FALSE, error);
  if (!retmap)
    return FALSE;
  return g_variant_new_from_bytes ((GVariantType *)variant_type, g_mapped_file_get_bytes (retmap),
                                   FALSE);
}

/**
 * rpm_ostree_get_basearch:
 *
 * Returns: A string for RPM's architecture, commonly used for e.g. $basearch in URLs
 * Since: 2017.8
 */
char *
rpm_ostree_get_basearch (void)
{
  g_autoptr (GError) local_error = NULL;
  char *args[] = { "get-basearch", NULL };
  g_autoptr (GVariant) ret = _rpmostree_shlib_ipc_send ("s", args, NULL, &local_error);
  g_assert_no_error (local_error);
  return g_variant_dup_string (ret, NULL);
}

/**
 * rpm_ostree_varsubst_basearch:
 * @src: String (commonly a URL)
 *
 * Returns: A copy of @src with all references for `${basearch}` replaced with
 * `rpmostree_get_basearch()`, or %NULL on error Since: 2017.8
 */
char *
rpm_ostree_varsubst_basearch (const char *src, GError **error)
{
  char *args[] = { "varsubst-basearch", (char *)src, NULL };
  g_autoptr (GVariant) ret = _rpmostree_shlib_ipc_send ("s", args, NULL, error);
  if (!ret)
    return NULL;
  return g_variant_dup_string (ret, NULL);
}

/**
 * rpm_ostree_check_version:
 * @required_year: Major/year required
 * @required_release: Release version required
 *
 * The `RPM_OSTREE_CHECK_VERSION` macro operates at compile time, whereas
 * this function operates at runtime.  The distinction is most useful for
 * things that are dynamic, such as scripting language callers.
 *
 * Returns: %TRUE if current library has at least the requested version, %FALSE otherwise
 */
gboolean
rpm_ostree_check_version (guint required_year, guint required_release)
{
  return RPM_OSTREE_CHECK_VERSION (required_year, required_release);
}
