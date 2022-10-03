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
#include <string.h>

#include "rpmostree-ex-builtins.h"
#include "rpmostree-libbuiltin.h"

#include "rpmostree-clientlib.h"
#include "rpmostree-container.h"

#include <libglnx.h>

gboolean
rpmostree_ex_builtin_rebuild (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                              GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("");

  if (!rpmostree_option_context_parse (context, NULL, &argc, &argv, invocation, cancellable, NULL,
                                       NULL, NULL, error))
    return FALSE;

  auto basearch = rpmostreecxx::get_rpm_basearch ();
  CXX_TRY_VAR (treefile, rpmostreecxx::treefile_new_client_from_etc (basearch), error);

  /* Right now we only support running this in a container */
  if (!rpmostree_container_rebuild (*treefile, cancellable, error))
    return FALSE;

  /* In the container flow, we effectively "consume" the treefiles after
   * modifying the rootfs. */
  CXX_TRY_VAR (n, rpmostreecxx::treefile_delete_client_etc (), error);
  if (n == 0)
    {
      g_print ("No changes to apply.\n");
    }

  return TRUE;
}
