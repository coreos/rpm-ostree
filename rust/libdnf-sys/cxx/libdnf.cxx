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

#include "libdnf.hxx"

namespace dnfcxx {
  // XXX: use macro to dedupe
  rust::String dnf_package_get_nevra(DnfPackage &pkg) {
    return rust::String(::dnf_package_get_nevra(&pkg));
  }
  rust::String dnf_package_get_name(DnfPackage &pkg) {
    return rust::String(::dnf_package_get_name(&pkg));
  }
  rust::String dnf_package_get_evr(DnfPackage &pkg) {
    return rust::String(::dnf_package_get_evr(&pkg));
  }
  rust::String dnf_package_get_arch(DnfPackage &pkg) {
    return rust::String(::dnf_package_get_arch(&pkg));
  }

  rust::String dnf_repo_get_id(DnfRepo &repo) {
    return rust::String(::dnf_repo_get_id(&repo));
  }
  guint64 dnf_repo_get_timestamp_generated(DnfRepo &repo) {
    return ::dnf_repo_get_timestamp_generated(&repo);
  }
}
