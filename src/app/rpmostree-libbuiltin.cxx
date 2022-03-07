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

#include "string.h"

#include "rpmostree-libbuiltin.h"
#include "rpmostree-util.h"
#include "rpmostree.h"

#include "libglnx.h"

void
rpmostree_print_kv_no_newline (const char *key, guint maxkeylen, const char *value)
{
  printf ("  %*s%s %s", maxkeylen, key, strlen (key) ? ":" : " ", value);
}

void
rpmostree_print_kv (const char *key, guint maxkeylen, const char *value)
{
  rpmostree_print_kv_no_newline (key, maxkeylen, value);
  putc ('\n', stdout);
}

void
rpmostree_usage_error (GOptionContext *context, const char *message, GError **error)
{
  g_assert (context != NULL);
  g_assert (message != NULL);

  g_autofree char *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s\n", help);

  (void)glnx_throw (error, "usage error: %s", message);
}

static const char *
get_id_from_deployment_variant (GVariant *deployment)
{
  g_autoptr (GVariantDict) dict = g_variant_dict_new (deployment);
  const char *id;
  g_assert (g_variant_dict_lookup (dict, "id", "&s", &id));
  return id;
}

gboolean
rpmostree_has_new_default_deployment (RPMOSTreeOS *os_proxy, GVariant *previous_deployment)
{
  g_autoptr (GVariant) new_deployment = rpmostree_os_dup_default_deployment (os_proxy);

  /* trivial case */
  if (g_variant_equal (previous_deployment, new_deployment))
    return FALSE;

  const char *previous_id = get_id_from_deployment_variant (previous_deployment);
  const char *new_id = get_id_from_deployment_variant (new_deployment);
  return !g_str_equal (previous_id, new_id);
}

namespace rpmostreecxx
{

/* Print the diff between the booted and pending deployments */
void
print_treepkg_diff_from_sysroot_path (rust::Str sysroot_path, RpmOstreeDiffPrintFormat format,
                                      guint32 max_key_len, GCancellable *cancellable)
{
  g_autoptr (GError) local_error = NULL;
  g_autofree char *sysroot_cpath = g_strndup (sysroot_path.data (), sysroot_path.length ());
  g_autoptr (GFile) sysroot_file = g_file_new_for_path (sysroot_cpath);
  g_autoptr (OstreeSysroot) sysroot = ostree_sysroot_new (sysroot_file);
  if (!ostree_sysroot_load (sysroot, cancellable, &local_error))
    util::throw_gerror (local_error);

  g_autoptr (GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  g_assert_cmpuint (deployments->len, >, 1);

  auto new_deployment = static_cast<OstreeDeployment *> (deployments->pdata[0]);
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  if (!booted_deployment || ostree_deployment_equal (booted_deployment, new_deployment))
    return;

  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, &local_error))
    util::throw_gerror (local_error);

  const char *from_rev = ostree_deployment_get_csum (booted_deployment);
  const char *to_rev = ostree_deployment_get_csum (new_deployment);

  g_autoptr (GPtrArray) removed = NULL;
  g_autoptr (GPtrArray) added = NULL;
  g_autoptr (GPtrArray) modified_old = NULL;
  g_autoptr (GPtrArray) modified_new = NULL;
  if (!rpm_ostree_db_diff (repo, from_rev, to_rev, &removed, &added, &modified_old, &modified_new,
                           cancellable, &local_error))
    util::throw_gerror (local_error);

  rpmostree_diff_print_formatted (format, NULL, max_key_len, removed, added, modified_old,
                                  modified_new);
}

} /* namespace */

void
rpmostree_print_timestamp_version (const char *version_string, const char *timestamp_string,
                                   guint max_key_len)
{
  if (!version_string)
    rpmostree_print_kv ("Timestamp", max_key_len, timestamp_string);
  else
    {
      g_autofree char *version_time = g_strdup_printf (
          "%s%s%s (%s)", get_bold_start (), version_string, get_bold_end (), timestamp_string);
      rpmostree_print_kv ("Version", max_key_len, version_time);
    }
}
