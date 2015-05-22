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

#include "config.h"

#include <string.h>
#include "rpmostree-refts.h"
#include "rpmostree-rpm-util.h"

/*
 * A wrapper for an `rpmts` that supports:
 *
 *  - Reference counting
 *  - Possibly holding a pointer to a tempdir, and cleaning it when unref'd
 */

RpmOstreeRefTs *
rpmostree_refts_new (rpmts ts, int temp_base_dfd, const char *temp_path)
{
  RpmOstreeRefTs *rts = g_new0 (RpmOstreeRefTs, 1);
  rts->ts = ts;
  rts->refcount = 1;
  rts->temp_base_dfd = temp_base_dfd;
  rts->temp_path = g_strdup (temp_path);
  return rts;
}

RpmOstreeRefTs *
rpmostree_refts_ref (RpmOstreeRefTs *rts)
{
  g_atomic_int_inc (&rts->refcount);
  return rts;
}

void
rpmostree_refts_unref (RpmOstreeRefTs *rts)
{
  if (!g_atomic_int_dec_and_test (&rts->refcount))
    return;
  rpmtsFree (rts->ts);
  
  if (rts->temp_path)
    (void) glnx_shutil_rm_rf_at (rts->temp_base_dfd, rts->temp_path, NULL, NULL);

  g_free (rts->temp_path);
  g_free (rts);
}
