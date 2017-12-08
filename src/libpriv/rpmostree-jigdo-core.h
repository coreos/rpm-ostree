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

/* An OIRPM is structured as an ordered set of files/directories; we use numeric
 * prefixes to ensure ordering. Most of the files are in GVariant format.
 *
 * An OIRPM starts with the OSTree commit object and its detached metadata,
 * so that can be GPG verified first - if that fails, we can then cleanly
 * abort.
 *
 * Next, we have the "jigdo set" - the NEVRAs + repodata checksum of the RPM
 * packages we need. These requires are also included in the RPM, but we also
 * have the repodata checksum here so that it's covered by the RPM GPG
 * signature, increasing security. The plan is to ensure that the repodata
 * checksums match the ones in this set.
 *
 * The dirmeta/dirtree objects that are referenced by the commit follow.
 *
 * A special optimization is made for "content-identical" new objects,
 * such as the initramfs right now which unfortunately has separate
 * SELinux labels and hence different object checksum.
 *
 * The pure added content objects follow - content objects which won't be
 * generated when we import the packages. One interesting detail is right now
 * this includes the /usr/lib/tmpfiles.d/pkg-foo.conf objects that we generate
 * server side, because we don't generate that client side in jigdo mode.
 *
 * Finally, we have the xattr data, which is mostly in support of SELinux
 * labeling (note this is done on the server side still).  In order to
 * dedup content, we have an xattr "string table" which is just an array
 * of xattrs; then there is a GVariant for each package which contains
 * a mapping of "objid" to an unsigned integer index into the xattr table.
 * The "objid" can either be a full path, or a basename if that basename is
 * unique inside a particular package.
 */

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

#define RPMOSTREE_JIGDO_PROVIDE_V1 "rpmostree-jigdo(v1)"

/* This one goes in the spec file to use as our replacement */
#define RPMOSTREE_JIGDO_SPEC_META_MAGIC "#@@@rpmostree_jigdo_meta@@@"
