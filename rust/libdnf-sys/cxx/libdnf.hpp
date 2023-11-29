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

#pragma once

#include <memory>

#include <gio/gio.h>
#include <libdnf/libdnf.h>

#include "rust/cxx.h"

namespace dnfcxx
{
typedef ::DnfPackage FFIDnfPackage;
class DnfPackage final
{
private:
  FFIDnfPackage *pkg;

public:
  DnfPackage (FFIDnfPackage *pkg) : pkg (pkg) {}
  ~DnfPackage () { g_clear_object (&pkg); }
  FFIDnfPackage &
  get_ref () noexcept
  {
    return *pkg;
  }
  rust::String
  get_nevra ()
  {
    return rust::String (::dnf_package_get_nevra (pkg));
  };
  rust::String
  get_name ()
  {
    return rust::String (::dnf_package_get_name (pkg));
  };
  rust::String
  get_evr ()
  {
    return rust::String (::dnf_package_get_evr (pkg));
  };
  rust::String
  get_arch ()
  {
    return rust::String (::dnf_package_get_arch (pkg));
  };
  rust::String
  get_sourcerpm ()
  {
    return rust::String (::dnf_package_get_sourcerpm (pkg));
  };
};

std::unique_ptr<DnfPackage> dnf_package_from_ptr (FFIDnfPackage *pkg) noexcept;

typedef ::DnfRepo FFIDnfRepo;
class DnfRepo final
{
private:
  FFIDnfRepo *repo;

public:
  DnfRepo (FFIDnfRepo *repo) : repo (repo) {}
  ~DnfRepo () { g_clear_object (&repo); }
  FFIDnfRepo &
  get_ref () noexcept
  {
    return *repo;
  }
  rust::String
  get_id ()
  {
    return rust::String (::dnf_repo_get_id (repo));
  };
  guint64
  get_timestamp_generated () noexcept
  {
    return ::dnf_repo_get_timestamp_generated (repo);
  };
};

std::unique_ptr<DnfRepo> dnf_repo_from_ptr (FFIDnfRepo *repo) noexcept;

typedef ::DnfSack FFIDnfSack;
class DnfSack final
{
private:
  FFIDnfSack *sack;

public:
  DnfSack (FFIDnfSack *sack) : sack (sack) {}
  ~DnfSack () { g_clear_object (&sack); }
  FFIDnfSack &
  get_ref () noexcept
  {
    return *sack;
  }
  std::unique_ptr<DnfPackage>
  add_cmdline_package (rust::String filename)
  {
    g_autoptr (DnfPackage) pkg = ::dnf_sack_add_cmdline_package (sack, filename.c_str ());
    if (pkg == NULL)
      throw std::runtime_error (std::string ("Invalid RPM file: ") + filename.c_str ());
    return dnf_package_from_ptr (pkg);
  };
};

std::unique_ptr<DnfSack> dnf_sack_new () noexcept;

// utility functions here
struct Nevra;
Nevra hy_split_nevra (rust::Str nevra);
}
