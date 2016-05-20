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
#include <libgsystem.h>
#include "rpmostree-util.h"
#include "rpmostree-refsack.h"
#include "rpmostree-refts.h"

#include "libglnx.h"

struct RpmHeaders
{
  RpmOstreeRefTs *refts; /* rpm transaction set the headers belong to */
  GPtrArray *hs; /* list of rpm header objects from <rpm.h> = Header */
};

struct RpmHeadersDiff
{
  GPtrArray *hs_add; /* list of rpm header objects from <rpm.h> = Header */
  GPtrArray *hs_del; /* list of rpm header objects from <rpm.h> = Header */
  GPtrArray *hs_mod_old; /* list of rpm header objects from <rpm.h> = Header */
  GPtrArray *hs_mod_new; /* list of rpm header objects from <rpm.h> = Header */
};

struct RpmRevisionData;

struct RpmHeadersDiff *
rpmhdrs_diff (struct RpmHeaders *l1,
              struct RpmHeaders *l2);

void
rpmhdrs_list (struct RpmHeaders *l1);

char *
rpmhdrs_rpmdbv (struct RpmHeaders *l1,
                GCancellable *cancellable,
                GError **error);

void
rpmhdrs_diff_prnt_block (struct RpmHeadersDiff *diff);

void
rpmhdrs_diff_prnt_diff (struct RpmHeadersDiff *diff);

struct RpmRevisionData *
rpmrev_new (OstreeRepo *repo,
            const char *rev,
            const GPtrArray *patterns,
            GCancellable *cancellable,
            GError **error);

struct RpmHeaders *rpmrev_get_headers (struct RpmRevisionData *self);

const char *rpmrev_get_commit (struct RpmRevisionData *self);

void
rpmrev_free (struct RpmRevisionData *ptr);

GS_DEFINE_CLEANUP_FUNCTION0(struct RpmRevisionData *, _cleanup_rpmrev_free, rpmrev_free);
#define _cleanup_rpmrev_ __attribute__((cleanup(_cleanup_rpmrev_free)))

gboolean
rpmostree_checkout_only_rpmdb_tempdir (OstreeRepo       *repo,
                                       const char       *ref,
                                       const char       *template,
                                       char            **out_tempdir,
                                       int              *out_tempdir_dfd,
                                       GCancellable     *cancellable,
                                       GError          **error);

RpmOstreeRefSack *
rpmostree_get_refsack_for_commit (OstreeRepo                *repo,
                                  const char                *ref,
                                  GCancellable              *cancellable,
                                  GError                   **error);

RpmOstreeRefSack *
rpmostree_get_refsack_for_root (int              dfd,
                                const char      *path,
                                GCancellable    *cancellable,
                                GError         **error);

gboolean
rpmostree_get_refts_for_commit (OstreeRepo                *repo,
                                const char                *ref,
                                RpmOstreeRefTs           **out_ts,
                                GCancellable              *cancellable,
                                GError                   **error);

gboolean
rpmostree_get_pkglist_for_root (int               dfd,
                                const char       *path,
                                RpmOstreeRefSack **out_refsack,
                                GPtrArray        **out_pkglist,
                                GCancellable     *cancellable,
                                GError          **error);

void
rpmostree_print_transaction (HifContext   *context);


void _rpmostree_reset_rpm_sighandlers (void);
