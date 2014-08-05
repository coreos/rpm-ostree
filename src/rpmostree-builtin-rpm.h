/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Anne LoVerso <anne.loverso@students.olin.edu>
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

#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include "rpmostree-util.h"

struct RpmRevisionData
{
  struct RpmHeaders *rpmdb;
  GFile *root;
  char *commit;
};

struct RpmHeadersDiff *
rpmhdrs_diff (struct RpmHeaders *l1,
              struct RpmHeaders *l2);

void
rpmhdrs_diff_prnt_diff (GFile *root1, GFile *root2, struct RpmHeadersDiff *diff,
                        GCancellable   *cancellable,
                        GError        **error);

void
rpmrev_free (struct RpmRevisionData *ptr);

_RPMOSTREE_DEFINE_TRIVIAL_CLEANUP_FUNC(struct RpmRevisionData *, rpmrev_free);

#define _cleanup_rpmrev_ __attribute__((cleanup(rpmrev_freep)))

struct RpmRevisionData *
rpmrev_new (OstreeRepo *repo, GFile *rpmdbdir, const char *rev,
      const GPtrArray *patterns,
      GCancellable   *cancellable,
      GError        **error);