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
pub enum FFIDnfPackage {}
unsafe impl ExternType for FFIDnfPackage {
    type Id = type_id!(dnfcxx::FFIDnfPackage);
    type Kind = cxx::kind::Trivial;
}

pub enum FFIDnfRepo {}
unsafe impl ExternType for FFIDnfRepo {
    type Id = type_id!(dnfcxx::FFIDnfRepo);
    type Kind = cxx::kind::Trivial;
}

pub enum FFIDnfSack {}
unsafe impl ExternType for FFIDnfSack {
    type Id = type_id!(dnfcxx::FFIDnfSack);
    type Kind = cxx::kind::Trivial;
}

#[cxx::bridge(namespace = "dnfcxx")]
pub mod ffi {

    struct Nevra {
        name: String,
        epoch: u64,
        version: String,
        release: String,
        arch: String,
    }

    unsafe extern "C++" {
        include!("libdnf.hpp");

        type DnfPackage;
        type FFIDnfPackage = crate::FFIDnfPackage;
        fn get_ref<'a>(self: Pin<&'a mut DnfPackage>) -> Pin<&'a mut FFIDnfPackage>;
        fn get_nevra(self: Pin<&mut DnfPackage>) -> String;
        fn get_name(self: Pin<&mut DnfPackage>) -> String;
        fn get_evr(self: Pin<&mut DnfPackage>) -> String;
        fn get_arch(self: Pin<&mut DnfPackage>) -> String;
        unsafe fn dnf_package_from_ptr(pkg: *mut FFIDnfPackage) -> UniquePtr<DnfPackage>;

        type DnfRepo;
        type FFIDnfRepo = crate::FFIDnfRepo;
        fn get_ref<'a>(self: Pin<&'a mut DnfRepo>) -> Pin<&'a mut FFIDnfRepo>;
        fn get_id(self: Pin<&mut DnfRepo>) -> String;
        fn get_timestamp_generated(self: Pin<&mut DnfRepo>) -> u64;
        unsafe fn dnf_repo_from_ptr(pkg: *mut FFIDnfRepo) -> UniquePtr<DnfRepo>;

        type DnfSack;
        type FFIDnfSack = crate::FFIDnfSack;
        fn get_ref<'a>(self: Pin<&'a mut DnfSack>) -> Pin<&'a mut FFIDnfSack>;
        fn add_cmdline_package(
            self: Pin<&mut DnfSack>,
            filename: String,
        ) -> Result<UniquePtr<DnfPackage>>;
        fn dnf_sack_new() -> UniquePtr<DnfSack>;

        fn hy_split_nevra(nevra: &str) -> Result<Nevra>;
    }
}

pub use ffi::*;

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    #[test]
    fn test_hy_split_nevra() {
        let n = hy_split_nevra("foobar-1.0-1.x86_64").unwrap();
        assert_eq!(n.name, "foobar");
        assert_eq!(n.epoch, 0);
        assert_eq!(n.version, "1.0");
        assert_eq!(n.release, "1");
        assert_eq!(n.arch, "x86_64");

        let n = hy_split_nevra("baz-boo-2:1.0.g123abc-3.mips").unwrap();
        assert_eq!(n.name, "baz-boo");
        assert_eq!(n.epoch, 2);
        assert_eq!(n.version, "1.0.g123abc");
        assert_eq!(n.release, "3");
        assert_eq!(n.arch, "mips");
    }
}
