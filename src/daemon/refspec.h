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

#ifndef RPM_OSTREED_REFSPEC_H__
#define RPM_OSTREED_REFSPEC_H__

#include "types.h"
#include "ostree.h"

G_BEGIN_DECLS

#define RPM_OSTREE_TYPE_DAEMON_REFSPEC  (refspec_get_type ())
#define REFSPEC(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), RPM_OSTREE_TYPE_DAEMON_REFSPEC, RefSpec))
#define RPM_OSTREE_IS_DAEMON_REFSPEC(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPM_OSTREE_TYPE_DAEMON_REFSPEC))

#define REFSPEC_DBUS_PATH_NAME "RefSpecs"


GType               refspec_get_type                (void) G_GNUC_CONST;

RPMOSTreeRefSpec *  refspec_new                     (Sysroot *sysroot,
                                                     const gchar *id);

gboolean            refspec_populate                (RefSpec *refspec,
                                                     const gchar *refspec_string,
                                                     OstreeRepo *repo,
                                                     gboolean publish);

gboolean            refspec_is_updating             (RefSpec *refspec);

gboolean            refspec_resolve_partial_aysnc   (Sysroot *sysroot,
                                                     const gchar *new_provided_refspec,
                                                     RefSpec *current_refspec,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data,
                                                     GError **error);

G_END_DECLS

#endif /* RPM_OSTREED_REFSPEC_H__ */
