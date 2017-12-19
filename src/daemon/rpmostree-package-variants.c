/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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

#include <rpmostree.h>
#include "rpmostree-package-variants.h"
#include <libglnx.h>

/**
 * package_to_variant
 * @package: RpmOstreePackage
 *
 * Returns: A floating GVariant of (sss) where values are
 * (package name, evr, arch)
 */
static GVariant *
package_variant_new (RpmOstreePackage *package)
{
  return g_variant_new ("(sss)",
                        rpm_ostree_package_get_name (package),
                        rpm_ostree_package_get_evr (package),
                        rpm_ostree_package_get_arch (package));
}


static GVariant *
build_diff_variant (const gchar *name,
                    guint type,
                    RpmOstreePackage *old_package,
                    RpmOstreePackage *new_package)
{
  GVariantBuilder options_builder;
  GVariantBuilder builder;
  g_variant_builder_init (&options_builder, G_VARIANT_TYPE ("a{sv}"));

  if (old_package)
    {
      g_variant_builder_add (&options_builder, "{sv}", "PreviousPackage",
                             package_variant_new (old_package));
    }

  if (new_package)
    {
      g_variant_builder_add (&options_builder, "{sv}", "NewPackage",
                             package_variant_new (new_package));
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
  g_variant_builder_add (&builder, "s", name);
  g_variant_builder_add (&builder, "u", type);
  g_variant_builder_add_value (&builder, g_variant_builder_end (&options_builder));
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}


static int
rpm_ostree_db_diff_variant_compare_by_name (const void *v1,
                                            const void *v2)

{
  GVariant **v1pp = (GVariant**)v1;
  GVariant *variant1 = *v1pp;
  GVariant **v2pp = (GVariant**)v2;
  GVariant *variant2 = *v2pp;

  const char *name1 = NULL;
  const char *name2 = NULL;
  g_variant_get_child (variant1, 0, "&s", &name1);
  g_variant_get_child (variant2, 0, "&s", &name2);

  g_assert (name1);
  g_assert (name2);
  return strcmp (name1, name2);
}

static int
rpm_ostree_db_diff_variant_compare_by_type (const void *v1,
                                            const void *v2)

{
  GVariant **v1pp = (GVariant**)v1;
  GVariant *variant1 = *v1pp;
  GVariant **v2pp = (GVariant**)v2;
  GVariant *variant2 = *v2pp;

  guint type1;
  guint type2;
  g_variant_get_child (variant1, 1, "u", &type1);
  g_variant_get_child (variant2, 1, "u", &type2);

  if (type1 == type2)
    return rpm_ostree_db_diff_variant_compare_by_name (v1, v2);

  return type1 - type2;
}

/**
 * rpm_ostree_db_build_diff_variant
 * @repo: A OstreeRepo
 * @old_ref: old ref to use
 * @new_ref: New ref to use
 * @out_variant: GVariant that represents the differences between the rpm
 *   databases on the given refs.
 * GCancellable: *cancellable
 * GError: **error
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
rpm_ostree_db_diff_variant (OstreeRepo *repo,
                            const char *from_rev,
                            const char *to_rev,
                            GVariant  **out_variant,
                            GCancellable *cancellable,
                            GError **error)
{

  g_autoptr(GPtrArray) removed = NULL;
  g_autoptr(GPtrArray) added = NULL;
  g_autoptr(GPtrArray) modified_old = NULL;
  g_autoptr(GPtrArray) modified_new = NULL;
  if (!rpm_ostree_db_diff (repo, from_rev, to_rev,
                           &removed, &added, &modified_old, &modified_new,
                           cancellable, error))
    return FALSE;

  g_assert_cmpuint (modified_old->len, ==, modified_new->len);

  g_autoptr(GPtrArray) found =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);

  for (guint i = 0; i < modified_old->len; i++)
    {
      guint type = RPM_OSTREE_PACKAGE_UPGRADED;
      RpmOstreePackage *oldpkg = modified_old->pdata[i];
      RpmOstreePackage *newpkg = modified_new->pdata[i];

      if (rpm_ostree_package_cmp (oldpkg, newpkg) > 0)
        type = RPM_OSTREE_PACKAGE_DOWNGRADED;

      const char *name = rpm_ostree_package_get_name (oldpkg);
      g_ptr_array_add (found, build_diff_variant (name, type, oldpkg, newpkg));
    }

  for (guint i = 0; i < removed->len; i++)
    {
      guint type = RPM_OSTREE_PACKAGE_REMOVED;
      RpmOstreePackage *pkg = removed->pdata[i];
      const char *name = rpm_ostree_package_get_name (pkg);
      g_ptr_array_add (found, build_diff_variant (name, type, pkg, NULL));
    }

  for (guint i = 0; i < added->len; i++)
    {
      guint type = RPM_OSTREE_PACKAGE_ADDED;
      RpmOstreePackage *pkg = added->pdata[i];
      const char *name = rpm_ostree_package_get_name (pkg);
      g_ptr_array_add (found, build_diff_variant (name, type, NULL, pkg));
    }

  g_ptr_array_sort (found, rpm_ostree_db_diff_variant_compare_by_type);

  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE_ARRAY);
  for (guint i = 0; i < found->len; i++)
    g_variant_builder_add_value (&builder, found->pdata[i]);

  if (found->len > 0)
    *out_variant = g_variant_builder_end (&builder);
  else
    *out_variant = g_variant_new ("a(sua{sv})", NULL);
  g_variant_ref_sink (*out_variant);
  return TRUE;
}
