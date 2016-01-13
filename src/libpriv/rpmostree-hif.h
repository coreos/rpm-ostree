/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include <gio/gio.h>
#include <libhif.h>
#include <libhif/hif-utils.h>

HifContext *_rpmostree_libhif_new_default (void);

gboolean _rpmostree_libhif_setup (HifContext    *context,
                                  GCancellable  *cancellable,
                                  GError       **error);

void _rpmostree_libhif_repos_disable_all (HifContext    *context);

gboolean _rpmostree_libhif_repos_enable_by_name (HifContext    *context,
                                                 const char    *name,
                                                 GError       **error);

gboolean _rpmostree_libhif_console_download_metadata (HifContext     *context,
                                                      GCancellable   *cancellable,
                                                      GError        **error);

gboolean _rpmostree_libhif_console_depsolve (HifContext     *context,
                                             GCancellable   *cancellable,
                                             GError        **error);
  
gboolean _rpmostree_libhif_console_download_content (HifContext     *context,
                                                     GCancellable   *cancellable,
                                                     GError        **error);
