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

#ifndef RPM_OSTREED_TYPES_H__
#define RPM_OSTREED_TYPES_H__

#include <glib-unix.h>
#include <gio/gio.h>

#include "rpm-ostreed-generated.h"

#include <stdint.h>
#include <string.h>

struct _Daemon;
typedef struct _Daemon Daemon;

struct _Manager;
typedef struct _Manager Manager;

struct _Deployment;
typedef struct _Deployment Deployment;

struct _RefSpec;
typedef struct _RefSpec RefSpec;

#endif /* RPM_OSTREED_TYPES_H__ */
