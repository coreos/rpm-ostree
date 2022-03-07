/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Red Hat, In.c
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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct RpmOstreePackage RpmOstreePackage;

#define RPM_OSTREE_TYPE_PACKAGE (rpm_ostree_package_get_type ())
#define RPM_OSTREE_PACKAGE(inst)                                                                   \
  (G_TYPE_CHECK_INSTANCE_CAST ((inst), RPM_OSTREE_TYPE_PACKAGE, RpmOstreePackage))
#define RPM_OSTREE_IS_PACKAGE(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), RPM_OSTREE_TYPE_PACKAGE))

_RPMOSTREE_EXTERN
GType rpm_ostree_package_get_type (void);

_RPMOSTREE_EXTERN
const char *rpm_ostree_package_get_nevra (RpmOstreePackage *p);

_RPMOSTREE_EXTERN
const char *rpm_ostree_package_get_name (RpmOstreePackage *p);

_RPMOSTREE_EXTERN
const char *rpm_ostree_package_get_evr (RpmOstreePackage *p);

_RPMOSTREE_EXTERN
const char *rpm_ostree_package_get_arch (RpmOstreePackage *p);

_RPMOSTREE_EXTERN
int rpm_ostree_package_cmp (RpmOstreePackage *p1, RpmOstreePackage *p2);

G_END_DECLS
