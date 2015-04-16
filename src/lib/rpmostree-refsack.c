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
#include "rpmostree-priv.h"

RpmOstreeRefSack *
_rpm_ostree_refsack_new (HySack sack)
{
  RpmOstreeRefSack *rsack = g_new0 (RpmOstreeRefSack, 1);
  rsack->sack = sack;
  rsack->refcount = 1;
  return rsack;
}

RpmOstreeRefSack *
_rpm_ostree_refsack_ref (RpmOstreeRefSack *rsack)
{
  g_atomic_int_inc (&rsack->refcount);
  return rsack;
}

void
_rpm_ostree_refsack_unref (RpmOstreeRefSack *rsack)
{
  if (!g_atomic_int_dec_and_test (&rsack->refcount))
    return;
  hy_sack_free (rsack->sack);
  g_free (rsack);
}
