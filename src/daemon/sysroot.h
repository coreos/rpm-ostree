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

#ifndef RPM_OSTREED_SYSROOT_H__
#define RPM_OSTREED_SYSROOT_H__

#include "types.h"
#include "ostree.h"

G_BEGIN_DECLS

#define RPM_OSTREE_TYPE_DAEMON_SYSROOT  (sysroot_get_type ())
#define SYSROOT(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), RPM_OSTREE_TYPE_DAEMON_SYSROOT, Sysroot))
#define RPM_OSTREE_IS_DAEMON_SYSROOT(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPM_OSTREE_TYPE_DAEMON_SYSROOT))

#define SYSROOT_DEFAULT_PATH "/"

GType                   sysroot_get_type                  (void) G_GNUC_CONST;

gboolean                sysroot_publish_new               (const gchar *path,
                                                           const gchar *dbus_name,
                                                           Sysroot **out_sysroot,
                                                           GError **error);

gchar *                 sysroot_generate_sub_object_path  (Sysroot *self,
                                                           const gchar *part,
                                                           ...);

gchar *                 sysroot_get_path                  (Sysroot *self);

gboolean                sysroot_track_client_auth         (GDBusInterfaceSkeleton *instance,
                                                           GDBusMethodInvocation *invocation,
                                                           gpointer user_data);

void                    sysroot_watch_client_if_needed    (Sysroot *sysroot,
                                                           GDBusConnection *conn,
                                                           const gchar *sender);

gboolean                sysroot_begin_update_operation    (Sysroot *self,
                                                           GDBusMethodInvocation *invocation,
                                                           const gchar *type);

void                    sysroot_end_update_operation      (Sysroot *self,
                                                           gboolean success,
                                                           const gchar *message,
                                                           gboolean wait_for_refresh);

G_END_DECLS

#endif /* RPM_OSTREED_SYSROOT_H__ */
