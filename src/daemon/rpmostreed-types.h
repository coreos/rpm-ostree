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

#include <gio/gio.h>
#include <glib-unix.h>

#include "rpm-ostreed-generated.h"

#include <stdint.h>
#include <string.h>

G_BEGIN_DECLS

struct _RpmostreedDaemon;
typedef struct _RpmostreedDaemon RpmostreedDaemon;

struct _RpmostreedSysroot;
typedef struct _RpmostreedSysroot RpmostreedSysroot;

struct _RpmostreedOS;
typedef struct _RpmostreedOS RpmostreedOS;
struct _RpmostreedOSExperimental;
typedef struct _RpmostreedOSExperimental RpmostreedOSExperimental;

struct _RpmostreedTransaction;
typedef struct _RpmostreedTransaction RpmostreedTransaction;

G_END_DECLS
