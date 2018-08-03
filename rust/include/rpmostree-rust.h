/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2018 Jonathan Lebon <jonathan@jlebon.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

typedef struct RpmOstreeRsTreefile RpmOstreeRsTreefile;

RpmOstreeRsTreefile *rpmostree_rs_treefile_new (const char *filename,
                                                const char *arch,
                                                int         workdir_dfd,
                                                GError    **error);

int rpmostree_rs_treefile_to_json (RpmOstreeRsTreefile *tf, GError **error);

const char *rpmostree_rs_treefile_get_rojig_spec_path (RpmOstreeRsTreefile *tf);

void rpmostree_rs_treefile_free (RpmOstreeRsTreefile *tf);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeRsTreefile, rpmostree_rs_treefile_free);
