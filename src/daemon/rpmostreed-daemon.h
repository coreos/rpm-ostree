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

#include "rpmostree-util.h"
#include "rpmostreed-types.h"

G_BEGIN_DECLS

#define RPMOSTREED_TYPE_DAEMON (rpmostreed_daemon_get_type ())
#define RPMOSTREED_DAEMON(o)                                                                       \
  (G_TYPE_CHECK_INSTANCE_CAST ((o), RPMOSTREED_TYPE_DAEMON, RpmostreedDaemon))
#define RPMOSTREED_IS_DAEMON(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPMOSTREED_TYPE_DAEMON))

#define DBUS_NAME "org.projectatomic.rpmostree1"
#define BASE_DBUS_PATH "/org/projectatomic/rpmostree1"

/* Update driver info */
#define RPMOSTREE_RUN_DIR "/run/rpm-ostree/"
#define RPMOSTREE_DRIVER_STATE RPMOSTREE_RUN_DIR "update-driver.gv"
#define RPMOSTREE_DRIVER_SD_UNIT "driver-sd-unit"
#define RPMOSTREE_DRIVER_NAME "driver-name"

GType rpmostreed_daemon_get_type (void) G_GNUC_CONST;
RpmostreedDaemon *rpmostreed_daemon_get (void);
GDBusConnection *rpmostreed_daemon_connection (void);
gboolean rpmostreed_get_client_uid (RpmostreedDaemon *self, const char *client, uid_t *out_uid);
void rpmostreed_daemon_add_client (RpmostreedDaemon *self, const char *client,
                                   const char *client_id);
void rpmostreed_daemon_remove_client (RpmostreedDaemon *self, const char *client);
char *rpmostreed_daemon_client_get_string (RpmostreedDaemon *self, const char *client);
char *rpmostreed_daemon_client_get_agent_id (RpmostreedDaemon *self, const char *client);
char *rpmostreed_daemon_client_get_sd_unit (RpmostreedDaemon *self, const char *client);
void rpmostreed_daemon_exit_now (RpmostreedDaemon *self);
void rpmostreed_daemon_reboot (RpmostreedDaemon *self);
gboolean rpmostreed_daemon_is_rebooting (RpmostreedDaemon *self);
void rpmostreed_daemon_run_until_idle_exit (RpmostreedDaemon *self);
void rpmostreed_daemon_publish (RpmostreedDaemon *self, const gchar *path, gboolean uniquely,
                                gpointer thing);
void rpmostreed_daemon_unpublish (RpmostreedDaemon *self, const gchar *path, gpointer thing);
gboolean rpmostreed_daemon_reload_config (RpmostreedDaemon *self, gboolean *out_changed,
                                          GError **error);

gboolean rpmostreed_authorize_method_for_uid0 (GDBusMethodInvocation *invocation);

RpmostreedAutomaticUpdatePolicy rpmostreed_get_automatic_update_policy (RpmostreedDaemon *self);

G_END_DECLS
