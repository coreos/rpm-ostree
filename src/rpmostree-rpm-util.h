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

#pragma once

#include <ostree.h>

#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include "rpmostree-util.h"

#include "libgsystem.h"

struct RpmHeaders
{
  rpmts ts; /* rpm transaction set the headers belong to */
  GPtrArray *hs; /* list of rpm header objects from <rpm.h> = Header */
};

struct RpmHeadersDiff
{
  GPtrArray *hs_add; /* list of rpm header objects from <rpm.h> = Header */
  GPtrArray *hs_del; /* list of rpm header objects from <rpm.h> = Header */
  GPtrArray *hs_mod_old; /* list of rpm header objects from <rpm.h> = Header */
  GPtrArray *hs_mod_new; /* list of rpm header objects from <rpm.h> = Header */
};

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
rpmhdrs_list (GFile *root,
              struct RpmHeaders *l1);

char *
rpmhdrs_rpmdbv (GFile *root,
                struct RpmHeaders *l1,
                GCancellable *cancellable,
                GError **error);

void
rpmhdrs_diff_prnt_block (GFile *root1,
                         GFile *root2,
                         struct RpmHeadersDiff *diff);

void
rpmhdrs_diff_prnt_diff (GFile *root1,
                        GFile *root2,
                        struct RpmHeadersDiff *diff);

struct RpmRevisionData *
rpmrev_new (OstreeRepo *repo,
            GFile *rpmdbdir,
            const char *rev,
            const GPtrArray *patterns,
            GCancellable *cancellable,
            GError **error);

void
rpmrev_free (struct RpmRevisionData *ptr);

GS_DEFINE_CLEANUP_FUNCTION0(struct RpmRevisionData *, _cleanup_rpmrev_free, rpmrev_free);
#define _cleanup_rpmrev_ __attribute__((cleanup(_cleanup_rpmrev_free)))

