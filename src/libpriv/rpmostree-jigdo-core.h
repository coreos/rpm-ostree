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

#pragma once

#include <ostree.h>

#include "libglnx.h"
#include <rpm/rpmlib.h>
#include <libdnf/libdnf.h>

/* Use a numeric prefix to ensure predictable ordering */
#define RPMOSTREE_JIGDO_COMMIT_DIR "00commit"
#define RPMOSTREE_JIGDO_PKGS "01pkgs"
#define RPMOSTREE_JIGDO_PKGS_VARIANT_FORMAT (G_VARIANT_TYPE ("a(stssss)")) // NEVRA,repodata checksum
#define RPMOSTREE_JIGDO_DIRMETA_DIR "02dirmeta"
#define RPMOSTREE_JIGDO_DIRTREE_DIR "03dirtree"
//#define RPMOSTREE_JIGDO_NEW_PKGIDENT "04new-pkgident"
//#define RPMOSTREE_JIGDO_NEW_PKGIDENT_VARIANT_FORMAT (G_VARIANT_TYPE ("a{ua{s(sa(uuua(ayay)))}}")) // Map<pkgid,Map<path,Set<(checksum,uid,gid,mode,xattrs)>)>>
#define RPMOSTREE_JIGDO_NEW_CONTENTIDENT_DIR "04new-contentident"
#define RPMOSTREE_JIGDO_NEW_CONTENTIDENT_VARIANT_FORMAT (G_VARIANT_TYPE ("a(suuua(ayay))")) // checksum,uid,gid,mode,xattrs
#define RPMOSTREE_JIGDO_NEW_DIR "05new"
#define RPMOSTREE_JIGDO_XATTRS_DIR "06xattrs"
#define RPMOSTREE_JIGDO_XATTRS_TABLE "06xattrs/00table"
#define RPMOSTREE_JIGDO_XATTRS_PKG_DIR "06xattrs/pkg"

#define RPMOSTREE_JIGDO_XATTRS_TABLE_VARIANT_FORMAT (G_VARIANT_TYPE ("aa(ayay)"))
/* NEVRA + xattr table */
#define RPMOSTREE_JIGDO_XATTRS_PKG_VARIANT_FORMAT (G_VARIANT_TYPE ("a(su)"))
