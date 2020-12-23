/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#![deny(unused_must_use)]

// pub(crate) utilities
mod cxx_bridge_gobject;
mod ffiutil;
pub use cxx_bridge_gobject::*;
mod includes;

#[cxx::bridge(namespace = "rpmostreecxx")]
mod ffi {
    // core.rs
    extern "Rust" {
        type TempEtcGuard;

        fn prepare_tempetc_guard(rootfs: i32) -> Result<Box<TempEtcGuard>>;
        fn undo(self: &TempEtcGuard) -> Result<()>;

        fn get_systemctl_wrapper() -> &'static [u8];
    }

    // initramfs.rs
    extern "Rust" {
        fn get_dracut_random_cpio() -> &'static [u8];
    }

    // scripts.rs
    extern "Rust" {
        fn script_is_ignored(pkg: &str, script: &str) -> bool;
    }

    // utils.rs
    extern "Rust" {
        fn download_to_fd(url: &str) -> Result<i32>;
    }

    #[derive(Default)]
    /// A copy of LiveFsState that is bridged to C++; the main
    /// change here is we can't use Option<> yet, so empty values
    /// are represented by the empty string.
    struct LiveApplyState {
        inprogress: String,
        commit: String,
    }

    extern "C++" {
        include!("src/libpriv/rpmostree-cxxrs-prelude.h");

        type OstreeSysroot = crate::FFIOstreeSysroot;
        type OstreeRepo = crate::FFIOstreeRepo;
        type OstreeDeployment = crate::FFIOstreeDeployment;
    }

    // livefs.rs
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
        fn transaction_livefs(sysroot: Pin<&mut OstreeSysroot>, target: &str) -> Result<()>;
    }
}

mod cliwrap;
pub use cliwrap::*;
mod composepost;
pub use self::composepost::*;
mod core;
use crate::core::*;
mod history;
pub use self::history::*;
mod journal;
pub use self::journal::*;
mod initramfs;
pub use self::initramfs::*;
mod lockfile;
pub use self::lockfile::*;
mod livefs;
pub(crate) use self::livefs::*;
// An origin parser in Rust but only built when testing until
// we're ready to try porting the C++ code.
#[cfg(test)]
mod origin;
mod ostree_diff;
mod ostree_utils;
mod progress;
pub use self::progress::*;
mod scripts;
pub(crate) use self::scripts::*;
mod testutils;
pub use self::testutils::*;
mod treefile;
pub use self::treefile::*;
mod utils;
pub use self::utils::*;
