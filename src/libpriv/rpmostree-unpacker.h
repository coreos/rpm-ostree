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

#include <ostree.h>

#include "libglnx.h"

typedef struct RpmOstreeUnpacker RpmOstreeUnpacker;

#define RPMOSTREE_TYPE_UNPACKER         (rpmostree_unpacker_get_type ())
#define RPMOSTREE_UNPACKER(inst)        (G_TYPE_CHECK_INSTANCE_CAST ((inst), RPMOSTREE_TYPE_UNPACKER, RpmOstreeUnpacker))
#define RPMOSTREE_IS_UNPACKER(inst)     (G_TYPE_CHECK_INSTANCE_TYPE ((inst), RPMOSTREE_TYPE_UNPACKER))

GType rpmostree_unpacker_get_type (void);

typedef enum {
  RPMOSTREE_UNPACKER_FLAGS_SUID_FSCAPS = (1 << 0),
  RPMOSTREE_UNPACKER_FLAGS_OWNER = (1 << 1)
} RpmOstreeUnpackerFlags;
#define RPMOSTREE_UNPACKER_FLAGS_ALL = (RPMOSTREE_UNPACKER_SUID_FSCAPS | RPMOSTREE_UNPACKER_OWNER)

RpmOstreeUnpacker *rpmostree_unpacker_new_fd (int fd, RpmOstreeUnpackerFlags flags, GError **error);

RpmOstreeUnpacker *rpmostree_unpacker_new_at (int dfd, const char *path, RpmOstreeUnpackerFlags flags, GError **error);

gboolean rpmostree_unpacker_unpack_to_dfd (RpmOstreeUnpacker *unpacker,
                                           int                dfd,
                                           GCancellable      *cancellable,
                                           GError           **error);
