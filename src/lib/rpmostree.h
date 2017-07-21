/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#ifndef _RPMOSTREE_EXTERN
#define _RPMOSTREE_EXTERN extern
#endif

#include <rpmostree-db.h>
#include <rpmostree-package.h>
#include <rpmostree-version.h>

G_BEGIN_DECLS

_RPMOSTREE_EXTERN
char *rpm_ostree_get_basearch (void);

_RPMOSTREE_EXTERN
char *rpm_ostree_varsubst_basearch (const char *src, GError **error);

_RPMOSTREE_EXTERN
gboolean rpm_ostree_check_version (guint required_year, guint required_release);

G_END_DECLS
