//! rpm-ostree a hybrid Rust and C/C++ application.  This is the
//! main library used by the executable, which also links to the
//! C/C++ `librpmostreeinternals.a` static library.

/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#![deny(unused_must_use)]
#![allow(clippy::ptr_arg)]

// pub(crate) utilities
mod cxxrsutil;
mod ffiutil;
pub(crate) use cxxrsutil::*;
mod includes;

/// APIs defined here are automatically bridged between Rust and C++ using https://cxx.rs/
///
/// Usage guidelines:
///
/// - Keep this content roughly ordered alphabetically
/// - While the return type here will be `Result<T>` on the implementation
///   side you currently *should* use `CxxResult`; see the docs of that for more information.
#[cxx::bridge(namespace = "rpmostreecxx")]
pub mod ffi {
    // Types that are defined by gtk-rs generated bindings that
    // we want to pass across the cxx-rs boundary.  For more
    // information, see cxx_bridge_gobject.rs.
    extern "C++" {
        include!("src/libpriv/rpmostree-cxxrs-prelude.h");

        type OstreeSysroot = crate::FFIOstreeSysroot;
        #[allow(dead_code)]
        type OstreeRepo = crate::FFIOstreeRepo;
        type OstreeDeployment = crate::FFIOstreeDeployment;
        type GCancellable = crate::FFIGCancellable;
        type GVariantDict = crate::FFIGVariantDict;
    }

    /// Currently cxx-rs doesn't support mappings; like probably most projects,
    /// by far our most common case is a mapping from string -> string and since
    /// our data sizes aren't large, we serialize this as a vector of strings pairs.
    /// In the future it's also likely that cxx-rs will support a C++ string_view
    /// so we could avoid duplicating in that direction.
    #[derive(Clone, Debug)]
    struct StringMapping {
        k: String,
        v: String,
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

    // daemon.rs
    extern "Rust" {
        fn deployment_generate_id(deployment: Pin<&mut OstreeDeployment>) -> String;
        fn deployment_populate_variant(
            mut sysroot: Pin<&mut OstreeSysroot>,
            mut deployment: Pin<&mut OstreeDeployment>,
            mut dict: Pin<&mut GVariantDict>,
        ) -> Result<()>;
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

    // progress.rs
    extern "Rust" {
        fn console_progress_begin_task(msg: &str);
        fn console_progress_begin_n_items(msg: &str, n: u64);
        fn console_progress_begin_percent(msg: &str);
        fn console_progress_set_message(msg: &str);
        fn console_progress_set_sub_message(msg: &str);
        fn console_progress_update(n: u64);
        fn console_progress_end(suffix: &str);
    }

    // history.rs

    /// A history entry in the journal. It may represent multiple consecutive boots
    /// into the same deployment. This struct is exposed directly via FFI to C.
    #[derive(PartialEq, Debug)]
    pub struct HistoryEntry {
        /// The deployment root timestamp.
        deploy_timestamp: u64,
        /// The command-line that was used to create the deployment, if any.
        deploy_cmdline: String,
        /// The number of consecutive times the deployment was booted.
        boot_count: u64,
        /// The first time the deployment was booted if multiple consecutive times.
        first_boot_timestamp: u64,
        /// The last time the deployment was booted if multiple consecutive times.
        last_boot_timestamp: u64,
        /// `true` if there are no more entries.
        eof: bool,
    }

    extern "Rust" {
        type HistoryCtx;

        fn history_ctx_new() -> Result<Box<HistoryCtx>>;
        fn next_entry(&mut self) -> Result<HistoryEntry>;
        fn history_prune() -> Result<()>;
    }

    // scripts.rs
    extern "Rust" {
        fn script_is_ignored(pkg: &str, script: &str) -> bool;
    }

    // testutils.rs
    extern "Rust" {
        fn testutils_entrypoint(argv: Vec<String>) -> Result<()>;
    }

    // treefile.rs
    extern "Rust" {
        type Treefile;

        fn treefile_new(filename: &str, basearch: &str, workdir: i32) -> Result<Box<Treefile>>;

        fn get_workdir(&self) -> i32;
        fn get_postprocess_script_fd(&mut self) -> i32;
        fn get_add_file_fd(&mut self, filename: &str) -> i32;
        fn get_passwd_fd(&mut self) -> i32;
        fn get_group_fd(&mut self) -> i32;
        fn get_json_string(&self) -> String;
        fn get_ostree_layers(&self) -> Vec<String>;
        fn get_ostree_override_layers(&self) -> Vec<String>;
        fn get_all_ostree_layers(&self) -> Vec<String>;
        fn get_repos(&self) -> Vec<String>;
        fn get_rojig_spec_path(&self) -> String;
        fn get_rojig_name(&self) -> String;
        fn get_cliwrap(&self) -> bool;
        fn get_readonly_executables(&self) -> bool;
        fn get_rpmdb(&self) -> String;
        fn get_files_remove_regex(&self, package: &str) -> Vec<String>;
        fn print_deprecation_warnings(&self);
        fn get_checksum(&self, repo: Pin<&mut OstreeRepo>) -> Result<String>;
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
        fn applylive_sync_ref(sysroot: Pin<&mut OstreeSysroot>) -> Result<()>;
        // FIXME/cxx make this Option<&str>
        fn transaction_apply_live(sysroot: Pin<&mut OstreeSysroot>, target: &str) -> Result<()>;
        fn applylive_client_finish() -> Result<()>;
    }

    // passwd.rs
    extern "Rust" {
        fn passwddb_open(rootfs: i32) -> Result<Box<PasswdDB>>;
        fn prepare_rpm_layering(rootfs: i32, merge_passwd_dir: &str) -> Result<bool>;
        fn complete_rpm_layering(rootfs: i32) -> Result<()>;
        fn passwd_cleanup(rootfs: i32) -> Result<()>;
        fn migrate_group_except_root(rootfs: i32, preserved_groups: &Vec<String>) -> Result<()>;
        fn migrate_passwd_except_root(rootfs: i32) -> Result<()>;
        fn passwd_compose_prep(rootfs: i32, treefile: &mut Treefile) -> Result<()>;
        fn passwd_compose_prep_repo(
            rootfs: i32,
            treefile: &mut Treefile,
            repo: Pin<&mut OstreeRepo>,
            previous_checksum: &str,
            unified_core: bool,
        ) -> Result<()>;
        fn dir_contains_uid(dirfd: i32, id: u32) -> Result<bool>;
        fn dir_contains_gid(dirfd: i32, id: u32) -> Result<bool>;

        type PasswdDB;
        fn lookup_user(self: &PasswdDB, uid: u32) -> Result<String>;
        fn lookup_group(self: &PasswdDB, gid: u32) -> Result<String>;
    }

    // extensions.rs
    extern "Rust" {
        type Extensions;
        fn extensions_load(
            path: &str,
            basearch: &str,
            base_pkgs: &Vec<StringMapping>,
        ) -> Result<Box<Extensions>>;
        fn get_repos(&self) -> Vec<String>;
        fn get_os_extension_packages(&self) -> Vec<String>;
        fn get_development_packages(&self) -> Vec<String>;
        fn state_checksum_changed(&self, chksum: &str, output_dir: &str) -> Result<bool>;
        fn update_state_checksum(&self, chksum: &str, output_dir: &str) -> Result<()>;
        fn serialize_to_dir(&self, output_dir: &str) -> Result<()>;
    }

    // lockfile.rs
    extern "Rust" {
        fn ror_lockfile_read(filenames: &Vec<String>) -> Result<Vec<StringMapping>>;
    }

    // rpmutils.rs
    extern "Rust" {
        fn cache_branch_to_nevra(nevra: &str) -> String;
    }

    unsafe extern "C++" {
        include!("rpmostree-util.h");
        // Currently only used in unit tests
        #[allow(dead_code)]
        fn util_next_version(
            auto_version_prefix: &str,
            version_suffix: &str,
            last_version: &str,
        ) -> Result<String>;
        fn testutil_validate_cxxrs_passthrough(repo: Pin<&mut OstreeRepo>) -> i32;
    }

    unsafe extern "C++" {
        include!("rpmostreemain.h");
        fn early_main();
        fn rpmostree_main(args: &[&str]) -> Result<()>;
        fn main_print_error(msg: &str);
    }

    unsafe extern "C++" {
        include!("rpmostree-diff.hpp");
        type RPMDiff;
        fn n_removed(&self) -> i32;
        fn n_added(&self) -> i32;
        fn n_modified(&self) -> i32;
        fn rpmdb_diff(
            repo: Pin<&mut OstreeRepo>,
            src: &CxxString,
            dest: &CxxString,
        ) -> Result<UniquePtr<RPMDiff>>;

        fn print(&self);
    }

    unsafe extern "C++" {
        include!("rpmostree-output.h");
        type Progress;

        fn progress_begin_task(msg: &str) -> UniquePtr<Progress>;
        fn end(self: Pin<&mut Progress>, msg: &str);

        fn output_message(msg: &str);
    }

    // rpmostree-rpm-util.h
    unsafe extern "C++" {
        include!("rpmostree-rpm-util.h");
        // Currently only used in unit tests
        #[allow(dead_code)]
        fn nevra_to_cache_branch(nevra: &CxxString) -> Result<UniquePtr<CxxString>>;
    }
}

mod client;
pub(crate) use client::*;
mod cliwrap;
pub use cliwrap::*;
mod composepost;
pub mod countme;
pub(crate) use composepost::*;
mod core;
use crate::core::*;
mod daemon;
mod dirdiff;
pub(crate) use daemon::*;
mod extensions;
pub(crate) use extensions::*;
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
mod nameservice;
// An origin parser in Rust but only built when testing until
// we're ready to try porting the C++ code.
#[cfg(test)]
mod origin;
mod ostree_diff;
mod ostree_utils;
mod passwd;
use passwd::*;
mod progress;
pub(crate) use self::progress::*;
mod scripts;
pub(crate) use self::scripts::*;
mod rpmutils;
pub(crate) use self::rpmutils::*;
mod testutils;
pub(crate) use self::testutils::*;
mod treefile;
pub use self::treefile::*;
mod utils;
pub use self::utils::*;
