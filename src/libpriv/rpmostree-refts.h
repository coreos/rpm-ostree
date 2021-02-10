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

#include <gio/gio.h>
#include <libdnf/libdnf.h>
#include "libglnx.h"

G_BEGIN_DECLS

typedef struct {
  gint refcount;  /* atomic */
  rpmts ts;
  GLnxTmpDir tmpdir;
} RpmOstreeRefTs;

RpmOstreeRefTs *
rpmostree_refts_new (rpmts ts, GLnxTmpDir *tmpdir);

RpmOstreeRefTs *
rpmostree_refts_ref (RpmOstreeRefTs *rts);

void
rpmostree_refts_unref (RpmOstreeRefTs *rts);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeRefTs, rpmostree_refts_unref);

G_END_DECLS
