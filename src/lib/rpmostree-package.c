/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:rpmostree-package
 * @short_description: An object representing a package
 * @include: rpmostree.h
 * @stability: Stable
 *
 * Represents an RPM package.
 */

#include "config.h"

#include "rpmostree-priv.h"

#include <string.h>
#include <stdlib.h>

typedef GObjectClass RpmOstreePackageClass;

struct RpmOstreePackage 
{
  GObject parent_instance;
  RpmOstreeRefSack *sack;
  HyPackage hypkg;
  char *cached_nevra;
};

G_DEFINE_TYPE(RpmOstreePackage, rpm_ostree_package, G_TYPE_OBJECT)

static void
rpm_ostree_package_finalize (GObject *object)
{
  RpmOstreePackage *pkg = (RpmOstreePackage*)object;
  free (pkg->cached_nevra);
  hy_package_free (pkg->hypkg);
  
  /* We do internal refcounting of the sack because hawkey doesn't */
  _rpm_ostree_refsack_unref (pkg->sack);

  G_OBJECT_CLASS (rpm_ostree_package_parent_class)->finalize (object);
}

static void
rpm_ostree_package_class_init (RpmOstreePackageClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = rpm_ostree_package_finalize;
}

static void
rpm_ostree_package_init (RpmOstreePackage *p)
{
}

/**
 * rpm_ostree_package_get_nevra:
 * @p: Package
 *
 * Returns: (transfer none): A formatted UTF-8 string containing the name, epoch, version,
 * release, and architecture.  Avoid parsing this; instead use
 * individual getters for more precise control.
 */
const char *
rpm_ostree_package_get_nevra (RpmOstreePackage *p)
{
  if (p->cached_nevra == NULL)
    p->cached_nevra = hy_package_get_nevra (p->hypkg);
  return p->cached_nevra;
}

/**
 * rpm_ostree_package_get_name:
 * @p: Package
 *
 * Returns: (transfer none): The package name
 */
const char *
rpm_ostree_package_get_name (RpmOstreePackage *p)
{
  return hy_package_get_name (p->hypkg);
}

/**
 * rpm_ostree_package_get_evr:
 * @p: Package
 *
 * Returns: (transfer none): The package epoch:version-release
 */
const char *
rpm_ostree_package_get_evr (RpmOstreePackage *p)
{
  return hy_package_get_evr (p->hypkg);
}

/**
 * rpm_ostree_package_get_arch:
 * @p: Package
 *
 * Returns: (transfer none): The package architecture
 */
const char *
rpm_ostree_package_get_arch (RpmOstreePackage *p)
{
  return hy_package_get_arch (p->hypkg);
}

RpmOstreePackage *
_rpm_ostree_package_new (RpmOstreeRefSack *rsack, HyPackage hypkg)
{
  RpmOstreePackage *p = g_object_new (RPM_OSTREE_TYPE_PACKAGE, NULL);
  p->sack = _rpm_ostree_refsack_ref (rsack);
  p->hypkg = hy_package_link (hypkg);
  return p;
}
