//! rpm-ostree is split into a C/C++ portion and a Rust portion, the latter
//! of which is compiled into a shared library, which is defined here.

/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#![deny(unused_must_use)]

// pub(crate) utilities
mod cxxrsutil;
mod ffiutil;
pub(crate) use cxxrsutil::*;
mod includes;

/// APIs defined here are automatically bridged between Rust and C++ using https://cxx.rs/
#[cxx::bridge(namespace = "rpmostreecxx")]
mod ffi {
    // Types that are defined by gtk-rs generated bindings that
    // we want to pass across the cxx-rs boundary.  For more
    // information, see cxx_bridge_gobject.rs.
    extern "C++" {
        include!("src/libpriv/rpmostree-cxxrs-prelude.h");

        type OstreeSysroot = crate::FFIOstreeSysroot;
        type OstreeRepo = crate::FFIOstreeRepo;
        type OstreeDeployment = crate::FFIOstreeDeployment;
        type GCancellable = crate::FFIGCancellable;
    }

    // client.rs
    extern "Rust" {
        fn client_handle_fd_argument(arg: &str, arch: &str) -> Result<Vec<i32>>;
    }

    // cliwrap.rs
    extern "Rust" {
        fn cliwrap_write_wrappers(rootfs: i32) -> Result<()>;
        fn cliwrap_entrypoint(argv: Vec<String>) -> Result<()>;
        fn cliwrap_destdir() -> String;
    }

    // core.rs
    extern "Rust" {
        type TempEtcGuard;

        fn prepare_tempetc_guard(rootfs: i32) -> Result<Box<TempEtcGuard>>;
        fn undo(self: &TempEtcGuard) -> Result<()>;

        fn get_systemctl_wrapper() -> &'static [u8];
    }

    // composepost.rs
    extern "Rust" {
        fn compose_postprocess_final(rootfs_dfd: i32) -> Result<()>;
    }

    // initramfs.rs
    extern "Rust" {
        fn get_dracut_random_cpio() -> &'static [u8];
        fn initramfs_overlay_generate(
            files: &Vec<String>,
            cancellable: Pin<&mut GCancellable>,
        ) -> Result<i32>;
    }

    // journal.rs
    extern "Rust" {
        fn journal_print_staging_failure();
    }

    // scripts.rs
    extern "Rust" {
        fn script_is_ignored(pkg: &str, script: &str) -> bool;
    }

    // testutils.rs
    extern "Rust" {
        fn testutils_entrypoint(argv: Vec<String>) -> Result<()>;
    }

    /// Currently cxx-rs doesn't support mappings; like probably most projects,
    /// by far our most common case is a mapping from string -> string and since
    /// our data sizes aren't large, we serialize this as a vector of strings pairs.
    #[derive(Clone, Debug)]
    struct StringMapping {
        k: String,
        v: String,
    }

    // utils.rs
    extern "Rust" {
        fn varsubstitute(s: &str, vars: &Vec<StringMapping>) -> Result<String>;
        fn get_features() -> Vec<String>;
        fn sealed_memfd(description: &str, content: &[u8]) -> Result<i32>;
    }

    #[derive(Default)]
    /// A copy of LiveFsState that is bridged to C++; the main
    /// change here is we can't use Option<> yet, so empty values
    /// are represented by the empty string.
    struct LiveApplyState {
        inprogress: String,
        commit: String,
    }

    // live.rs
    extern "Rust" {
        fn get_live_apply_state(
            sysroot: Pin<&mut OstreeSysroot>,
            deployment: Pin<&mut OstreeDeployment>,
        ) -> Result<LiveApplyState>;
        fn has_live_apply_state(
            sysroot: Pin<&mut OstreeSysroot>,
            deployment: Pin<&mut OstreeDeployment>,
        ) -> Result<bool>;
        // FIXME/cxx make this Option<&str>
        fn transaction_apply_live(sysroot: Pin<&mut OstreeSysroot>, target: &str) -> Result<()>;
    }

    // passwd.rs
    extern "Rust" {
        fn passwddb_open(rootfs: i32) -> Result<Box<PasswdDB>>;

        type PasswdDB;
        fn add_user(self: &mut PasswdDB, uid: u32, username: &str);
        fn lookup_user(self: &PasswdDB, uid: u32) -> Result<String>;
        fn add_group(self: &mut PasswdDB, gid: u32, groupname: &str);
        fn lookup_group(self: &PasswdDB, gid: u32) -> Result<String>;
        // TODO(lucab): get rid of the two methods below.
        fn add_group_content(self: &mut PasswdDB, rootfs: i32, group_path: &str) -> Result<()>;
        fn add_passwd_content(self: &mut PasswdDB, rootfs: i32, passwd_path: &str) -> Result<()>;
    }

    // countme.rs
    extern "Rust" {
        fn countme_entrypoint(argv: Vec<String>) -> Result<()>;
    }
}

mod client;
pub(crate) use client::*;
mod cliwrap;
pub use cliwrap::*;
mod countme;
pub(crate) use countme::*;
mod composepost;
pub(crate) use composepost::*;
mod core;
use crate::core::*;
mod dirdiff;
#[cfg(feature = "fedora-integration")]
mod fedora_integration;
mod history;
pub use self::history::*;
mod journal;
pub(crate) use self::journal::*;
mod initramfs;
pub(crate) use self::initramfs::*;
mod lockfile;
pub use self::lockfile::*;
mod live;
pub(crate) use self::live::*;
// An origin parser in Rust but only built when testing until
// we're ready to try porting the C++ code.
#[cfg(test)]
mod origin;
mod ostree_diff;
mod ostree_utils;
mod passwd;
use passwd::{passwddb_open, PasswdDB};
mod progress;
pub use self::progress::*;
mod scripts;
pub(crate) use self::scripts::*;
mod testutils;
pub(crate) use self::testutils::*;
mod treefile;
pub use self::treefile::*;
mod utils;
pub use self::utils::*;
