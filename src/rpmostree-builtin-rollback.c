/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "rpmostree-builtins.h"
#include "rpmostree-treepkgdiff.h"

#include "libgsystem.h"

static gboolean opt_reboot;

static GOptionEntry option_entries[] = {
  { "reboot", 'r', 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a reboot after rollback is prepared", NULL },
  { NULL }
};

gboolean
rpmostree_builtin_rollback (int             argc,
                            char          **argv,
                            GCancellable   *cancellable,
                            GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("- Revert to the previously booted tree");
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gs_free char *origin_description = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_unref_ptrarray GPtrArray *new_deployments =
    g_ptr_array_new_with_free_func (g_object_unref);
  OstreeDeployment *booted_deployment = NULL;
  guint i;
  guint booted_index;
  guint index_to_prepend;
  
  g_option_context_add_main_entries (context, option_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  sysroot = ostree_sysroot_new_default ();
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  if (booted_deployment == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system");
      goto out;
    }

  deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployments->len < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Found %u deployments, at least 2 required for rollback",
                   deployments->len);
      goto out;
    }

  g_assert (booted_deployment != NULL);
  for (booted_index = 0; booted_index < deployments->len; booted_index++)
    {
      if (deployments->pdata[booted_index] == booted_deployment)
        break;
    }
  g_assert (booted_index < deployments->len);
  g_assert (deployments->pdata[booted_index] == booted_deployment);
  
  if (booted_index != 0)
    {
      /* There is an earlier deployment, let's assume we want to just
       * insert the current one in front.
       */

       /*
       What this does is, if we're NOT in the default boot index, it plans to prepend
       our current index (1, since we can't have more than two trees) so that it becomes index 0 
       (default) and the current default becomes index 1
       */
      index_to_prepend = booted_index;
    }
  else
    {
      /* We're booted into the first, let's roll back to the previous */
      index_to_prepend = 1;
    }
  
  g_ptr_array_add (new_deployments, g_object_ref (deployments->pdata[index_to_prepend]));
  for (i = 0; i < deployments->len; i++)
    {
      if (i == index_to_prepend)
        continue;
      g_ptr_array_add (new_deployments, g_object_ref (deployments->pdata[i]));
    }

  g_print ("Moving '%s.%d' to be first deployment\n",
           ostree_deployment_get_csum (deployments->pdata[index_to_prepend]),
           ostree_deployment_get_deployserial (deployments->pdata[index_to_prepend]));

  if (!ostree_sysroot_write_deployments (sysroot, new_deployments, cancellable,
                                         error))
    goto out;

  if (opt_reboot)
    gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                   cancellable, error,
                                   "systemctl", "reboot", NULL);
  else
    {
#ifdef HAVE_PATCHED_HAWKEY_AND_LIBSOLV
      if (!rpmostree_print_treepkg_diff (sysroot, cancellable, error))
        goto out;
#endif

      g_print ("Sucessfully reset deployment order; run \"systemctl reboot\" to start a reboot\n");
    }

  if (opt_reboot)
  ret = TRUE;
 out:
  return ret;
}
