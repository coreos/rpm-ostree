/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <hawkey/packagelist.h>
#include <hawkey/query.h>
#include <hawkey/sack.h>
#include <hawkey/stringarray.h>
#include <hawkey/goal.h>
#include <hawkey/version.h>
#include <hawkey/util.h>
#include "hif-utils.h"

#include "rpmostree-util.h"

#include "libgsystem.h"

/* This one is in libgsystem git master, but not 2014.2 */
GS_DEFINE_CLEANUP_FUNCTION0(GKeyFile*, _cleanup_g_key_file_unref, g_key_file_unref);
#define _cleanup_gkeyfile_ __attribute__((cleanup(_cleanup_g_key_file_unref)))

GS_DEFINE_CLEANUP_FUNCTION0(HySack, _cleanup_hy_sack_free, hy_sack_free);
GS_DEFINE_CLEANUP_FUNCTION0(HyQuery, _cleanup_hy_query_free, hy_query_free);
GS_DEFINE_CLEANUP_FUNCTION0(HyPackageList, _cleanup_hy_packagelist_free, hy_packagelist_free);
GS_DEFINE_CLEANUP_FUNCTION0(HyStringArray, _cleanup_hy_stringarray_free, hy_stringarray_free);

static inline void
cleanup_closep (int *fdp)
{
  int fd;

  g_assert (fdp);

  fd = *fdp;
  if (fd != -1)
    (void) close (fd);
}

#define _cleanup_close_ __attribute__((cleanup(cleanup_closep)))
#define _cleanup_hysack_ __attribute__((cleanup(_cleanup_hy_sack_free)))
#define _cleanup_hyquery_ __attribute__((cleanup(_cleanup_hy_query_free)))
#define _cleanup_hypackagelist_ __attribute__((cleanup(_cleanup_hy_packagelist_free)))
#define _cleanup_hystringarray_ __attribute__((cleanup(_cleanup_hy_stringarray_free)))
