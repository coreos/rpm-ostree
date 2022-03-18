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

#include <glib-unix.h>
#include <string.h>

#include "rpmostree-builtins.h"
#include "rpmostree-clientlib.h"
#include "rpmostree-core.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-util.h"

#include <libglnx.h>

static char *opt_osname;
static gboolean opt_base;
static gboolean opt_pending;
static gboolean opt_rollback;
static gboolean opt_repomd;

static GOptionEntry option_entries[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Operate on provided OSNAME", "OSNAME" },
  { "base", 'b', 0, G_OPTION_ARG_NONE, &opt_base,
    "Clear temporary files; will leave deployments unchanged", NULL },
  { "pending", 'p', 0, G_OPTION_ARG_NONE, &opt_pending, "Remove pending deployment", NULL },
  { "rollback", 'r', 0, G_OPTION_ARG_NONE, &opt_rollback, "Remove rollback deployment", NULL },
  { "repomd", 'm', 0, G_OPTION_ARG_NONE, &opt_repomd, "Delete cached rpm repo metadata", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_cleanup (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("");
  g_autoptr (GPtrArray) cleanup_types = g_ptr_array_new ();
  glnx_unref_object RPMOSTreeOS *os_proxy = NULL;
  glnx_unref_object RPMOSTreeSysroot *sysroot_proxy = NULL;
  g_autofree char *transaction_address = NULL;

  if (!rpmostree_option_context_parse (context, option_entries, &argc, &argv, invocation,
                                       cancellable, NULL, NULL, &sysroot_proxy, error))
    return FALSE;

  if (argc < 1 || argc > 2)
    {
      rpmostree_usage_error (context, "Too few or too many arguments", error);
      return FALSE;
    }

  if (opt_base)
    g_ptr_array_add (cleanup_types, (char *)"base");
  if (opt_pending)
    g_ptr_array_add (cleanup_types, (char *)"pending-deploy");
  if (opt_rollback)
    g_ptr_array_add (cleanup_types, (char *)"rollback-deploy");
  if (opt_repomd)
    g_ptr_array_add (cleanup_types, (char *)"repomd");
  if (cleanup_types->len == 0)
    {
      glnx_throw (error, "At least one cleanup option must be specified");
      return FALSE;
    }

  auto is_ostree_container = ROSCXX_TRY_VAL (is_ostree_container (), error);
  if (is_ostree_container && cleanup_types->len == 1 && opt_repomd)
    {
      /* just directly nuke the cachedir */
      return glnx_shutil_rm_rf_at (AT_FDCWD, RPMOSTREE_CORE_CACHEDIR, cancellable, error);
    }

  g_ptr_array_add (cleanup_types, NULL);

  if (!rpmostree_load_os_proxy (sysroot_proxy, opt_osname, cancellable, &os_proxy, error))
    return FALSE;

  if (!rpmostree_os_call_cleanup_sync (os_proxy, (const char *const *)cleanup_types->pdata,
                                       &transaction_address, cancellable, error))
    return FALSE;

  if (!rpmostree_transaction_get_response_sync (sysroot_proxy, transaction_address, cancellable,
                                                error))
    return FALSE;

  return TRUE;
}
