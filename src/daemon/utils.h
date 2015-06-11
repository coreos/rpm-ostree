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

#include <ostree.h>

#include "types.h"

gchar *    utils_generate_object_path     (const gchar  *base,
                                           const gchar  *part,
                                           ...);

gchar *    utils_generate_object_path_from_va   (const gchar *base,
                                                 const gchar  *part,
                                                 va_list va);

gboolean   utils_load_sysroot_and_repo          (gchar *path,
                                                 GCancellable *cancellable,
                                                 OstreeSysroot **out_sysroot,
                                                 OstreeRepo **out_repo,
                                                 GError **error);

gboolean   refspec_parse_partial                (const gchar *new_provided_refspec,
                                                 gchar *base_refspec,
                                                 gchar **out_refspec,
                                                 GError **error);
