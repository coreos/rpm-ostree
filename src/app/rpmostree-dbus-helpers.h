/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <glib-unix.h>
#include <gio/gio.h>

#include "rpm-ostreed-generated.h"

#include <stdint.h>
#include <string.h>
#include <ostree.h>

#define BUS_NAME "org.projectatomic.rpmostree1"

gboolean
rpmostree_load_connection_and_manager        (gchar *sysroot,
                                              gboolean force_peer,
                                              GCancellable *cancellable,
                                              GDBusConnection **out_connection,
                                              RPMOSTreeManager **out_manager,
                                              gboolean *out_is_peer,
                                              GError **error);

gboolean
rpmostree_is_valid_object_path               (gchar *string);

gboolean
rpmostree_deployment_deploy_sync             (RPMOSTreeManager *manager,
                                              RPMOSTreeDeployment *deployment,
                                              GCancellable *cancellable,
                                              GError **error);

gboolean
rpmostree_refspec_update_sync                (RPMOSTreeManager *manager,
                                              RPMOSTreeRefSpec *refspec,
                                              const gchar *method,
                                              GVariant *parameters,
                                              GCancellable *cancellable,
                                              GError **error);

void
rpmostree_print_signatures                   (GVariant *variant,
                                              const gchar *sep);
