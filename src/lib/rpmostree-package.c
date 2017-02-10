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

#include "rpmostree-package-priv.h"

#include <string.h>
#include <stdlib.h>

typedef GObjectClass RpmOstreePackageClass;

struct RpmOstreePackage 
{
  GObject parent_instance;
  RpmOstreeRefSack *sack;
  DnfPackage *hypkg;
};

G_DEFINE_TYPE(RpmOstreePackage, rpm_ostree_package, G_TYPE_OBJECT)

static void
rpm_ostree_package_finalize (GObject *object)
{
  RpmOstreePackage *pkg = (RpmOstreePackage*)object;
  g_object_unref (pkg->hypkg);
  
  /* We do internal refcounting of the sack because hawkey doesn't */
  rpmostree_refsack_unref (pkg->sack);

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
  return dnf_package_get_nevra (p->hypkg);
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
  return dnf_package_get_name (p->hypkg);
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
  return dnf_package_get_evr (p->hypkg);
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
  return dnf_package_get_arch (p->hypkg);
}

/**
 * rpm_ostree_package_cmp:
 * @p1: Package
 * @p2: Package
 *
 * Compares two packages by name, epoch:version-release and architecture.
 *
 * Returns: an integer suitable for sorting functions; negative if @p1 should
 *          sort before @p2 in name or version, 0 if equal, positive if @p1
 *          should sort after @p2
 */
int
rpm_ostree_package_cmp (RpmOstreePackage *p1, RpmOstreePackage *p2)
{
  return dnf_package_cmp (p1->hypkg, p2->hypkg);
}

RpmOstreePackage *
_rpm_ostree_package_new (RpmOstreeRefSack *rsack, DnfPackage *hypkg)
{
  RpmOstreePackage *p = g_object_new (RPM_OSTREE_TYPE_PACKAGE, NULL);
  p->sack = rpmostree_refsack_ref (rsack);
  p->hypkg = g_object_ref (hypkg);
  return p;
}
