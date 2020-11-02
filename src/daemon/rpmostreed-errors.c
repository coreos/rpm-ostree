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

#include "rpmostreed-errors.h"
#include "rpmostreed-types.h"

static const GDBusErrorEntry dbus_error_entries[] =
{
  {RPM_OSTREED_ERROR_FAILED,
   "org.projectatomic.rpmostreed.Error.Failed"},
  {RPM_OSTREED_ERROR_INVALID_SYSROOT,
   "org.projectatomic.rpmostreed.Error.InvalidSysroot"},
  {RPM_OSTREED_ERROR_NOT_AUTHORIZED,
   "org.projectatomic.rpmostreed.Error.NotAuthorized"},
  {RPM_OSTREED_ERROR_UPDATE_IN_PROGRESS,
   "org.projectatomic.rpmostreed.Error.UpdateInProgress"},
  {RPM_OSTREED_ERROR_INVALID_REFSPEC,
   "org.projectatomic.rpmostreed.Error.InvalidRefspec"},
};

GQuark
rpmostreed_error_quark (void)
{
  G_STATIC_ASSERT (G_N_ELEMENTS (dbus_error_entries) == RPM_OSTREED_ERROR_NUM_ENTRIES);
  static gsize quark = 0;
  g_dbus_error_register_error_domain ("rpmostreed-error-quark",
                                      &quark,
                                      dbus_error_entries,
                                      G_N_ELEMENTS (dbus_error_entries));
  return (GQuark) quark;
}
