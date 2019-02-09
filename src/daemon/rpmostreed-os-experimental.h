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

#pragma once

#include "rpmostreed-types.h"

#define RPMOSTREED_TYPE_OSEXPERIMENTAL   (rpmostreed_osexperimental_get_type ())
#define RPMOSTREED_OSEXPERIMENTAL(o)     (G_TYPE_CHECK_INSTANCE_CAST ((o), RPMOSTREED_TYPE_OSEXPERIMENTAL, RpmostreedOSExperimental))
#define RPMOSTREED_IS_OSEXPERIMENTAL(o)  (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPMOSTREED_TYPE_OSEXPERIMENTAL))

GType             rpmostreed_osexperimental_get_type           (void) G_GNUC_CONST;
RPMOSTreeOSExperimental *     rpmostreed_osexperimental_new                (OstreeSysroot *sysroot,
                                                                OstreeRepo *repo,
                                                                            const char *name);
