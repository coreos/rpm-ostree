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

#include <glib-unix.h>
#include <rpm/rpmsq.h>
#include <gio/gunixoutputstream.h>

#include "rpmostree-hif.h"
#include "rpmostree-cleanup.h"

HifContext *
_rpmostree_libhif_get_default (void)
{
  HifContext *hifctx;

  /* We can be control-c'd at any time */
#if BUILDOPT_HAVE_RPMSQ_SET_INTERRUPT_SAFETY
  rpmsqSetInterruptSafety (FALSE);
#endif

  hifctx = hif_context_new ();
  hif_context_set_http_proxy (hifctx, g_getenv ("http_proxy"));

  hif_context_set_repo_dir (hifctx, "/etc/yum.repos.d");
  hif_context_set_cache_dir (hifctx, "/var/cache/rpm-ostree/metadata");
  hif_context_set_solv_dir (hifctx, "/var/cache/rpm-ostree/solv");
  hif_context_set_lock_dir (hifctx, "/run/rpm-ostree/lock");

  hif_context_set_check_disk_space (hifctx, FALSE);
  hif_context_set_check_transaction (hifctx, FALSE);
  hif_context_set_yumdb_enabled (hifctx, FALSE);

  return hifctx;
}

gboolean
_rpmostree_libhif_setup (HifContext    *context,
                         GCancellable  *cancellable,
                         GError       **error)
{
  if (!hif_context_setup (context, cancellable, error))
    return FALSE;

  /* Forcibly override rpm/librepo SIGINT handlers.  We always operate
   * in a fully idempotent/atomic mode, and can be killed at any time.
   */
  signal (SIGINT, SIG_DFL);
  signal (SIGTERM, SIG_DFL);
  
  return TRUE;
}

void
_rpmostree_libhif_repos_disable_all (HifContext    *context)
{
  GPtrArray *sources;
  guint i;

  sources = hif_context_get_sources (context);
  for (i = 0; i < sources->len; i++)
    {
      HifSource *src = sources->pdata[i];
      
      hif_source_set_enabled (src, HIF_SOURCE_ENABLED_NONE);
    }
}

gboolean
_rpmostree_libhif_repos_enable_by_name (HifContext    *context,
                                        const char    *name,
                                        GError       **error)
{
  gboolean ret = FALSE;
  GPtrArray *sources;
  guint i;
  gboolean found = FALSE;

  sources = hif_context_get_sources (context);
  for (i = 0; i < sources->len; i++)
    {
      HifSource *src = sources->pdata[i];
      const char *id = hif_source_get_id (src);

      if (strcmp (name, id) != 0)
        continue;
      
      hif_source_set_enabled (src, HIF_SOURCE_ENABLED_PACKAGES);
#ifdef HAVE_HIF_SOURCE_SET_REQUIRED
      hif_source_set_required (src, TRUE);
#endif
      found = TRUE;
      break;
    }

  if (!found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unknown rpm-md repository: %s", name);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
