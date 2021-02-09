/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use cxx::{type_id, ExternType};

/* This is an experiment in calling libdnf functions from Rust. Really, it'd be more sustainable to
 * generate a "libdnf-sys" from SWIG (though it doesn't look like that's supported yet
 * https://github.com/swig/swig/issues/1468) or at least `bindgen` for the strict C parts of the
 * API. For now, I just took the shortcut of manually defining a tiny subset we care about. */

// This technique for an uninstantiable/opaque type is used by libgit2-sys at least:
// https://github.com/rust-lang/git2-rs/blob/master/libgit2-sys/lib.rs#L51
// XXX: dedupe with macro
pub enum DnfPackage {}
unsafe impl ExternType for DnfPackage {
    type Id = type_id!(dnfcxx::DnfPackage);
    type Kind = cxx::kind::Trivial;
}

pub enum DnfRepo {}
unsafe impl ExternType for DnfRepo {
    type Id = type_id!(dnfcxx::DnfRepo);
    type Kind = cxx::kind::Trivial;
}

#[cxx::bridge(namespace = "dnfcxx")]
pub mod ffi {
    unsafe extern "C++" {
        include!("libdnf.hxx");

        type DnfPackage = crate::DnfPackage;
        fn dnf_package_get_nevra(pkg: &mut DnfPackage) -> Result<String>;
        fn dnf_package_get_name(pkg: &mut DnfPackage) -> Result<String>;
        fn dnf_package_get_evr(pkg: &mut DnfPackage) -> Result<String>;
        fn dnf_package_get_arch(pkg: &mut DnfPackage) -> Result<String>;

        type DnfRepo = crate::DnfRepo;
        fn dnf_repo_get_id(repo: &mut DnfRepo) -> Result<String>;
        fn dnf_repo_get_timestamp_generated(repo: &mut DnfRepo) -> Result<u64>;
    }
}

pub use ffi::*;
