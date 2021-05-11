/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <glib.h>

#include "libtest.h"

/* Copied and adapted from:
 * https://github.com/ostreedev/ostree/blob/main/tests/libostreetest.c
 *
 * This function hovers in a quantum superposition of horrifying and
 * beautiful.  Future generations may interpret it as modern art.
 */
gboolean
rot_test_run_libtest (const char *cmd, GError **error)
{
  const char *srcdir = g_getenv ("topsrcdir");
  int estatus;
  g_autoptr(GPtrArray) argv = g_ptr_array_new ();
  g_autoptr(GString) cmdstr = g_string_new ("");

  g_ptr_array_add (argv, (char*)"bash");
  g_ptr_array_add (argv, (char*)"-c");

  g_string_append (cmdstr, "set -xeuo pipefail; . ");
  g_string_append (cmdstr, srcdir);
  g_string_append (cmdstr, "/tests/common/libtest.sh; ");
  g_string_append (cmdstr, cmd);

  g_ptr_array_add (argv, (char*)cmdstr->str);
  g_ptr_array_add (argv, NULL);

  if (!g_spawn_sync (NULL, (char**)argv->pdata, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, NULL, NULL, &estatus, error))
    return FALSE;

  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;

  return TRUE;
}

