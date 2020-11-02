/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
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

#include <libdnf/libdnf.h>
#include "libglnx.h"

typedef struct {
  gint refcount;  /* atomic */
  DnfSack *sack;
  GLnxTmpDir tmpdir;
} RpmOstreeRefSack;

RpmOstreeRefSack *
rpmostree_refsack_new (DnfSack *sack, GLnxTmpDir *tmpdir);

RpmOstreeRefSack *
rpmostree_refsack_ref (RpmOstreeRefSack *rsack);

void
rpmostree_refsack_unref (RpmOstreeRefSack *rsack);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeRefSack, rpmostree_refsack_unref);

