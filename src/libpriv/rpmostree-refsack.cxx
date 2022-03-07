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

#include "rpmostree-refsack.h"
#include "rpmostree-rpm-util.h"
#include <string.h>

RpmOstreeRefSack *
rpmostree_refsack_new (DnfSack *sack, GLnxTmpDir *tmpdir)
{
  RpmOstreeRefSack *rsack = g_new0 (RpmOstreeRefSack, 1);
  rsack->sack = (DnfSack *)g_object_ref (sack);
  rsack->refcount = 1;
  if (tmpdir)
    {
      rsack->tmpdir = *tmpdir;
      tmpdir->initialized = FALSE; /* Steal ownership */
    }
  return rsack;
}

RpmOstreeRefSack *
rpmostree_refsack_ref (RpmOstreeRefSack *rsack)
{
  g_atomic_int_inc (&rsack->refcount);
  return rsack;
}

void
rpmostree_refsack_unref (RpmOstreeRefSack *rsack)
{
  if (!g_atomic_int_dec_and_test (&rsack->refcount))
    return;
  g_object_unref (rsack->sack);

  /* The sack might point to a temporarily allocated rpmdb copy, if so,
   * prune it now.
   */
  (void)glnx_tmpdir_delete (&rsack->tmpdir, NULL, NULL);
  g_free (rsack);
}
