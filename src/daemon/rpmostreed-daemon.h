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

#define RPMOSTREED_TYPE_DAEMON   (rpmostreed_daemon_get_type ())
#define RPMOSTREED_DAEMON(o)     (G_TYPE_CHECK_INSTANCE_CAST ((o), RPMOSTREED_TYPE_DAEMON, RpmostreedDaemon))
#define RPMOSTREED_IS_DAEMON(o)  (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPMOSTREED_TYPE_DAEMON))

#define DBUS_NAME "org.projectatomic.rpmostree1"
#define BASE_DBUS_PATH "/org/projectatomic/rpmostree1"

GType              rpmostreed_daemon_get_type       (void) G_GNUC_CONST;
RpmostreedDaemon * rpmostreed_daemon_get            (void);
void               rpmostreed_daemon_add_client     (RpmostreedDaemon *self, const char *client);
void               rpmostreed_daemon_remove_client  (RpmostreedDaemon *self, const char *client);
gboolean           rpmostreed_daemon_has_client     (RpmostreedDaemon *self, const char *client);
void               rpmostreed_daemon_exit_now       (RpmostreedDaemon *self);
void               rpmostreed_daemon_run_until_idle_exit (RpmostreedDaemon *self);
void               rpmostreed_daemon_publish        (RpmostreedDaemon *self,
                                                     const gchar *path,
                                                     gboolean uniquely,
                                                     gpointer thing);
void               rpmostreed_daemon_unpublish      (RpmostreedDaemon *self,
                                                     const gchar *path,
                                                     gpointer thing);
gboolean           rpmostreed_reload_config         (RpmostreedDaemon *self,
						     GError          **error);
