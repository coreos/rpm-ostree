/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

use libc;
use glib_sys;

/* This is an experiment in calling libdnf functions from Rust. Really, it'd be more sustainable to
 * generate a "libdnf-sys" from SWIG (though it doesn't look like that's supported yet
 * https://github.com/swig/swig/issues/1468) or at least `bindgen` for the strict C parts of the
 * API. For now, I just took the shortcut of manually defining a tiny subset we care about. */

#[repr(C)]
pub(crate) struct DnfPackage(libc::c_void);

#[allow(dead_code)]
extern {
    pub(crate) fn dnf_package_get_nevra(package: *mut DnfPackage) -> *const libc::c_char;
    pub(crate) fn dnf_package_get_name(package: *mut DnfPackage) -> *const libc::c_char;
    pub(crate) fn dnf_package_get_evr(package: *mut DnfPackage) -> *const libc::c_char;
    pub(crate) fn dnf_package_get_arch(package: *mut DnfPackage) -> *const libc::c_char;
}

/* And some helper rpm-ostree C functions to deal with libdnf stuff. These are prime candidates for
 * oxidation since it makes e.g. interacting with strings less efficient. */
extern {
    pub(crate) fn rpmostree_get_repodata_chksum_repr(package: *mut DnfPackage, chksum: *mut *mut libc::c_char, gerror: *mut *mut glib_sys::GError) -> libc::c_int;
}
