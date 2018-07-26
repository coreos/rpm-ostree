/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

#include <ostree.h>

#include "libglnx.h"

typedef enum {
  RPMOSTREE_BWRAP_IMMUTABLE = 0,
  RPMOSTREE_BWRAP_MUTATE_ROFILES,
  RPMOSTREE_BWRAP_MUTATE_FREELY
} RpmOstreeBwrapMutability;

typedef struct RpmOstreeBwrap RpmOstreeBwrap;
RpmOstreeBwrap *rpmostree_bwrap_ref (RpmOstreeBwrap *bwrap);
void rpmostree_bwrap_unref (RpmOstreeBwrap *bwrap);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeBwrap, rpmostree_bwrap_unref)

/* TODO - move this utility elsewhere */
void rpmostree_ptrarray_append_strdup (GPtrArray *argv_array, ...) G_GNUC_NULL_TERMINATED;

RpmOstreeBwrap *rpmostree_bwrap_new_base (int rootfs, GError **error);

RpmOstreeBwrap *rpmostree_bwrap_new (int rootfs,
                                     RpmOstreeBwrapMutability mutable,
                                     GError **error,
                                     ...) G_GNUC_NULL_TERMINATED;

void rpmostree_bwrap_set_inherit_stdin (RpmOstreeBwrap *bwrap);
void rpmostree_bwrap_append_bwrap_argv (RpmOstreeBwrap *bwrap, ...) G_GNUC_NULL_TERMINATED;
void rpmostree_bwrap_append_child_argv (RpmOstreeBwrap *bwrap, ...) G_GNUC_NULL_TERMINATED;
void rpmostree_bwrap_append_child_argva (RpmOstreeBwrap *bwrap, int argc, char **argv);

void rpmostree_bwrap_setenv (RpmOstreeBwrap *bwrap, const char *name, const char *value);

void rpmostree_bwrap_set_child_setup (RpmOstreeBwrap *bwrap,
                                      GSpawnChildSetupFunc func,
                                      gpointer             data);

gboolean rpmostree_bwrap_run (RpmOstreeBwrap *bwrap,
                              GCancellable   *cancellable,
                              GError        **error);

gboolean rpmostree_bwrap_selftest (GError **error);
