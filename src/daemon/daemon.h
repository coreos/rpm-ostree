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

#ifndef RPM_OSTREED_DAEMON_H__
#define RPM_OSTREED_DAEMON_H__

#include "types.h"

G_BEGIN_DECLS

#define TYPE_DAEMON   (daemon_get_type ())
#define DAEMON(o)     (G_TYPE_CHECK_INSTANCE_CAST ((o), TYPE_DAEMON, Daemon))
#define IS_DAEMON(o)  (G_TYPE_CHECK_INSTANCE_TYPE ((o), TYPE_DAEMON))

#define DBUS_NAME "org.projectatomic.rpmostree1"
#define BASE_DBUS_PATH "/org/projectatomic/rpmostree1"

GType                      daemon_get_type           (void) G_GNUC_CONST;

Daemon *                   daemon_get                (void);

gboolean                   daemon_on_message_bus     (Daemon *self);

Daemon *                   daemon_new                (GDBusConnection *connection,
                                                      gboolean persist);

void                       daemon_hold               (Daemon *self);

void                       daemon_release            (Daemon *self);

void                       daemon_publish            (Daemon *self,
                                                      const gchar *path,
                                                      gboolean uniquely,
                                                      gpointer thing);

void                       daemon_unpublish          (Daemon *self,
                                                      const gchar *path,
                                                      gpointer thing);

GTask *                    daemon_get_new_task        (Daemon *self,
                                                       gpointer source_object,
                                                       GCancellable *cancellable,
                                                       GAsyncReadyCallback callback,
                                                       gpointer callback_data);

GDBusInterface *           daemon_get_interface       (Daemon *self,
                                                       const gchar *object_path,
                                                       const gchar *interface_name);

G_END_DECLS

#endif /* RPM_OSTREED_DAEMON_H__ */
