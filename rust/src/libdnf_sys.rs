/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use libc;
use glib_sys;

/* This is an experiment in calling libdnf functions from Rust. Really, it'd be more sustainable to
 * generate a "libdnf-sys" from SWIG (though it doesn't look like that's supported yet
 * https://github.com/swig/swig/issues/1468) or at least `bindgen` for the strict C parts of the
 * API. For now, I just took the shortcut of manually defining a tiny subset we care about. */

pub(crate) enum DnfPackage {}
pub(crate) enum DnfRepo {}

#[allow(dead_code)]
extern {
    pub(crate) fn dnf_package_get_nevra(package: *mut DnfPackage) -> *const libc::c_char;
    pub(crate) fn dnf_package_get_name(package: *mut DnfPackage) -> *const libc::c_char;
    pub(crate) fn dnf_package_get_evr(package: *mut DnfPackage) -> *const libc::c_char;
    pub(crate) fn dnf_package_get_arch(package: *mut DnfPackage) -> *const libc::c_char;

    pub(crate) fn dnf_repo_get_id(repo: *mut DnfRepo) -> *const libc::c_char;
    pub(crate) fn dnf_repo_get_timestamp_generated(repo: *mut DnfRepo) -> u64;
}

/* And some helper rpm-ostree C functions to deal with libdnf stuff. These are prime candidates for
 * oxidation since it makes e.g. interacting with strings less efficient. */
extern {
    pub(crate) fn rpmostree_get_repodata_chksum_repr(package: *mut DnfPackage, chksum: *mut *mut libc::c_char, gerror: *mut *mut glib_sys::GError) -> libc::c_int;
}
