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

#ifndef RPM_OSTREED_AUTH_H__
#define RPM_OSTREED_AUTH_H__

#include "types.h"

G_BEGIN_DECLS

gboolean    auth_check_root_or_access_denied     (GDBusInterfaceSkeleton *instance,
                                                  GDBusMethodInvocation *invocation,
                                                  gpointer user_data);

G_END_DECLS

#endif /* RPM_OSTREED_AUTH_H__ */
