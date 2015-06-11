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

#define TYPE_SYSROOT   (sysroot_get_type ())
#define SYSROOT(o)     (G_TYPE_CHECK_INSTANCE_CAST ((o), TYPE_SYSROOT, Sysroot))
#define IS_SYSROOT(o)  (G_TYPE_CHECK_INSTANCE_TYPE ((o), TYPE_SYSROOT))

#define SYSROOT_DEFAULT_PATH "/"

GType             sysroot_get_type                  (void) G_GNUC_CONST;

Sysroot *         sysroot_get                       (void);

gchar *           sysroot_get_sysroot_path          (Sysroot *self);

gboolean          sysroot_populate                  (Sysroot *self,
                                                     GError **error);

void              sysroot_emit_update               (Sysroot *self,
                                                     OstreeSysroot *ot_sysroot);

G_END_DECLS

#endif /* RPM_OSTREED_SYSROOT_H__ */
