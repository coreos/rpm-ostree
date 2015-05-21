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

#ifndef RPM_OSTREED_DEPLOYMENT_H__
#define RPM_OSTREED_DEPLOYMENT_H__

#include "types.h"
#include "ostree.h"

G_BEGIN_DECLS

#define RPM_OSTREE_TYPE_DAEMON_DEPLOYMENT  (deployment_get_type ())
#define DEPLOYMENT(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), RPM_OSTREE_TYPE_DAEMON_DEPLOYMENT, Deployment))
#define RPM_OSTREE_IS_DAEMON_DEPLOYMENT(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPM_OSTREE_TYPE_DAEMON_DEPLOYMENT))

#define DEPLOYMENT_DBUS_PATH_NAME "Deployments"

GType                   deployment_get_type     (void) G_GNUC_CONST;

RPMOSTreeDeployment *   deployment_new          (gchar *id);

gboolean                deployment_populate     (Deployment *deployment,
                                                 OstreeDeployment *ostree_deployment,
                                                 OstreeRepo *repo,
                                                 gboolean publish);

RefSpec *               deployment_get_refspec  (Deployment *self);

char *                  deployment_generate_id  (OstreeDeployment *ostree_deployment);

gint                    deployment_index_compare (gconstpointer a,
                                                  gconstpointer b);

G_END_DECLS

#endif /* RPM_OSTREED_DEPLOYMENT_H__ */
