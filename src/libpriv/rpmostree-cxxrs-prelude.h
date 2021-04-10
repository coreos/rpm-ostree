/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2020 Red Hat, Inc.
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

namespace rpmostreecxx {
    // Currently cxx-rs requires that external bindings are in the same namespace as
    // its own bindings, so we maintain typedefs.  Update cxx_bridge_gobject.rs first.
    typedef ::OstreeSysroot OstreeSysroot;
    typedef ::OstreeRepo OstreeRepo;
    typedef ::OstreeDeployment OstreeDeployment;
    typedef ::GObject GObject;
    typedef ::GCancellable GCancellable;
    typedef ::GDBusConnection GDBusConnection;
    typedef ::GVariant GVariant;
    typedef ::GVariantDict GVariantDict;
    typedef ::GKeyFile GKeyFile;
}

// XXX: really should just include! libdnf.hxx in the bridge
#include <libdnf/libdnf.h>
namespace dnfcxx {
  typedef ::DnfPackage DnfPackage;
  typedef ::DnfRepo DnfRepo;
}
