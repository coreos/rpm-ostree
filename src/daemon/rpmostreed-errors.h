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

G_BEGIN_DECLS

#define RPM_OSTREED_ERROR (rpmostreed_error_quark ())

typedef enum {
  RPM_OSTREED_ERROR_FAILED,
  RPM_OSTREED_ERROR_INVALID_SYSROOT,
  RPM_OSTREED_ERROR_NOT_AUTHORIZED,
  RPM_OSTREED_ERROR_UPDATE_IN_PROGRESS,
  RPM_OSTREED_ERROR_INVALID_REFSPEC,
  RPM_OSTREED_ERROR_NUM_ENTRIES,
} RpmOstreedError;

GQuark rpmostreed_error_quark (void);

G_END_DECLS
