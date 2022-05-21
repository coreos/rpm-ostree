/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2021 Red Hat, Inc.
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

#include <glib-unix.h>

#include "libdnf-sys/lib.rs.h"
#include "libdnf.hpp"

namespace dnfcxx
{
std::unique_ptr<DnfPackage>
dnf_package_from_ptr (FFIDnfPackage *pkg) noexcept
{
  return std::make_unique<DnfPackage> (g_object_ref (pkg));
}

std::unique_ptr<DnfRepo>
dnf_repo_from_ptr (FFIDnfRepo *repo) noexcept
{
  return std::make_unique<DnfRepo> (g_object_ref (repo));
}

// XXX: dedupe with rpmostree_decompose_nevra
Nevra
hy_split_nevra (rust::Str nevra)
{
  g_autofree char *name = NULL;
  int epoch;
  g_autofree char *version = NULL;
  g_autofree char *release = NULL;
  g_autofree char *arch = NULL;

  g_autofree char *nevra_c = g_strndup (nevra.data (), nevra.length ());
  if (::hy_split_nevra (nevra_c, &name, &epoch, &version, &release, &arch) != 0)
    throw std::runtime_error (std::string ("Failed to decompose NEVRA string: ") + nevra_c);

  Nevra r = {
    rust::String (name),    (guint64)epoch,      rust::String (version),
    rust::String (release), rust::String (arch),
  };
  // the copy here *should* get elided
  return r;
}
}
