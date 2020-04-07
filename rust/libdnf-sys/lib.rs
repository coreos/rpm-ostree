/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use libc;

/* This is an experiment in calling libdnf functions from Rust. Really, it'd be more sustainable to
 * generate a "libdnf-sys" from SWIG (though it doesn't look like that's supported yet
 * https://github.com/swig/swig/issues/1468) or at least `bindgen` for the strict C parts of the
 * API. For now, I just took the shortcut of manually defining a tiny subset we care about. */

// This technique for an uninstantiable/opaque type is used by libgit2-sys at least:
// https://github.com/rust-lang/git2-rs/blob/master/libgit2-sys/lib.rs#L51
pub enum DnfPackage {}
pub enum DnfRepo {}

extern "C" {
    pub fn dnf_package_get_nevra(package: *mut DnfPackage) -> *const libc::c_char;
    pub fn dnf_package_get_name(package: *mut DnfPackage) -> *const libc::c_char;
    pub fn dnf_package_get_evr(package: *mut DnfPackage) -> *const libc::c_char;
    pub fn dnf_package_get_arch(package: *mut DnfPackage) -> *const libc::c_char;

    pub fn dnf_repo_get_id(repo: *mut DnfRepo) -> *const libc::c_char;
    pub fn dnf_repo_get_timestamp_generated(repo: *mut DnfRepo) -> u64;
}
