/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2018 Colin Walters <walters@verbum.org>
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
#include <systemd/sd-journal.h>
#include <systemd/sd-daemon.h>
#include <stdio.h>
#include <libglnx.h>
#include <glib/gi18n.h>
#include <syslog.h>
#include "libglnx.h"

#include "rpmostree-builtins.h"
#include "rpmostree-util.h"
#include "rpmostreed-daemon.h"
#include "rpmostree-rust.h"
#include "rpmostree-libbuiltin.h"

gboolean
rpmostree_builtin_multiproc_worker (int             argc,
                                    char          **argv,
                                    RpmOstreeCommandInvocation *invocation,
                                    GCancellable   *cancellable,
                                    GError        **error)
{
  g_autoptr(GPtrArray) args = g_ptr_array_new ();
  for (int i = 0; i < argc; i++)
    g_ptr_array_add (args, argv[i]);
  g_ptr_array_add (args, NULL);
  ror_multiproc_entrypoint ((char**)args->pdata);

  return TRUE;
}
