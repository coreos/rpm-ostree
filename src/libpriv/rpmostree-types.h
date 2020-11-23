/* Copyright (C) 2018 Red Hat, Inc.
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

#pragma once

G_BEGIN_DECLS

/* These types are used by both the daemon and the client. Some of them should probably be
 * added to the public API eventually. */

typedef enum {
  RPM_OSTREE_PKG_TYPE_BASE,
  RPM_OSTREE_PKG_TYPE_LAYER,
} RpmOstreePkgTypes;

typedef enum {
  RPMOSTREED_AUTOMATIC_UPDATE_POLICY_NONE,
  RPMOSTREED_AUTOMATIC_UPDATE_POLICY_CHECK,
  RPMOSTREED_AUTOMATIC_UPDATE_POLICY_STAGE,
} RpmostreedAutomaticUpdatePolicy;

typedef enum {
  RPM_OSTREE_ADVISORY_SEVERITY_NONE,
  RPM_OSTREE_ADVISORY_SEVERITY_LOW,
  RPM_OSTREE_ADVISORY_SEVERITY_MODERATE,
  RPM_OSTREE_ADVISORY_SEVERITY_IMPORTANT,
  RPM_OSTREE_ADVISORY_SEVERITY_CRITICAL,
  RPM_OSTREE_ADVISORY_SEVERITY_LAST,
} RpmOstreeAdvisorySeverity;

/**
 * RPMOSTREE_DIFF_SINGLE_GVARIANT_FORMAT:
 *
 * - u - type (RpmOstreePkgTypes)
 * - s - name
 * - s - evr
 * - s - arch
 */
#define RPMOSTREE_DIFF_SINGLE_GVARIANT_STRING "a(usss)"
#define RPMOSTREE_DIFF_SINGLE_GVARIANT_FORMAT G_VARIANT_TYPE (RPMOSTREE_DIFF_SINGLE_GVARIANT_STRING)

/**
 * RPMOSTREE_DIFF_MODIFIED_GVARIANT_FORMAT:
 *
 * - u - type (RpmOstreePkgTypes)
 * - s - name
 * - (ss) - evr & arch of previous package
 * - (ss) - evr & arch of new package
 */
#define RPMOSTREE_DIFF_MODIFIED_GVARIANT_STRING "a(us(ss)(ss))"
#define RPMOSTREE_DIFF_MODIFIED_GVARIANT_FORMAT G_VARIANT_TYPE (RPMOSTREE_DIFF_MODIFIED_GVARIANT_STRING)

G_END_DECLS
