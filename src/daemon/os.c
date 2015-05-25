/*
* Copyright (C) 2015 Red Hat, Inc.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "config.h"
#include "ostree.h"

#include "types.h"
#include "os.h"

typedef struct _OSStubClass OSStubClass;

struct _OSStub
{
  RPMOSTreeOSSkeleton parent_instance;
};

struct _OSStubClass
{
  RPMOSTreeOSSkeletonClass parent_class;
};

static void osstub_iface_init (RPMOSTreeOSIface *iface);


G_DEFINE_TYPE_WITH_CODE (OSStub, osstub, RPMOSTREE_TYPE_OS_SKELETON,
                         G_IMPLEMENT_INTERFACE (RPMOSTREE_TYPE_OS,
                                                osstub_iface_init));

static void
osstub_init (OSStub *self)
{
}

static void
osstub_class_init (OSStubClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
osstub_iface_init (RPMOSTreeOSIface *iface)
{
}


/* ---------------------------------------------------------------------------------------------------- */
