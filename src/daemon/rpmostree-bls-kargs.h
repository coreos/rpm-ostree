/*
 * Copyright (C) 2026 Red Hat, Inc.
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

G_BEGIN_DECLS

/**
 * rpmostree_bls_compute_source_diff:
 * @old_kargs: The current/old kargs string (can be NULL)
 * @new_kargs: The new kargs string (can be NULL)
 * @out_to_add: (out) (transfer full): Kargs to add
 * @out_to_remove: (out) (transfer full): Kargs to remove
 *
 * Computes the diff between old and new source kargs.
 * Both strings are space-separated lists of kernel arguments.
 *
 * Returns: TRUE if there are any changes, FALSE if identical
 */
gboolean rpmostree_bls_compute_source_diff (const char *old_kargs, const char *new_kargs,
                                            char ***out_to_add, char ***out_to_remove);

G_END_DECLS
