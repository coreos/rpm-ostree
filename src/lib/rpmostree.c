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

#include "string.h"

#include "rpmostree.h"
#include "rpmostree-core.h"
#include "rpmostree-util.h"

/**
 * SECTION:librpmostree
 * @title: Global high level APIs
 * @short_description: APIs for accessing global state
 *
 * These APIs access generic global state.
 */

/**
 * rpm_ostree_get_basearch:
 *
 * Returns: A string for RPM's architecture, commonly used for e.g. $basearch in URLs
 * Since: 2017.8
 */
char *
rpm_ostree_get_basearch (void)
{
  g_autoptr(DnfContext) ctx = dnf_context_new ();
  /* Need to strdup since we unref the context */
  return g_strdup (dnf_context_get_base_arch (ctx));
}

/**
 * rpm_ostree_varsubst_basearch:
 * @src: String (commonly a URL)
 *
 * Returns: A copy of @src with all references for `${basearch}` replaced with `rpmostree_get_basearch()`, or %NULL on error
 * Since: 2017.8
 */
char *
rpm_ostree_varsubst_basearch (const char *src, GError **error)
{
  g_autoptr(DnfContext) ctx = dnf_context_new ();
  g_autoptr(GHashTable) varsubsts = rpmostree_dnfcontext_get_varsubsts (ctx);
  return _rpmostree_varsubst_string (src, varsubsts, error);
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
  return RPM_OSTREE_CHECK_VERSION(required_year, required_release);
}
