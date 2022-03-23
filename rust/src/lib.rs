//! rpm-ostree is a hybrid Rust and C/C++ application. This is the
//! main library used by the executable, which also links to the
//! C/C++ `librpmostreeinternals.a` static library.

/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
// See https://doc.rust-lang.org/rustc/lints/listing/allowed-by-default.html
#![deny(missing_debug_implementations)]
#![deny(unsafe_op_in_unsafe_fn)]
#![forbid(unused_must_use)]
#![allow(clippy::ptr_arg)]

// pub(crate) utilities
mod cxxrsutil;
mod ffiutil;
pub(crate) use cxxrsutil::*;

/// APIs defined here are automatically bridged between Rust and C++ using https://cxx.rs/
///
/// # File layout
///
/// Try to keep APIs defined here roughly alphabetical.  When adding a new file,
/// add a comment with the filename so that it shows up in searches too.
///
/// # Error handling
///
/// For fallible APIs that return a `Result<T>`:
///
/// - Use `Result<T>` inside `lib.rs` below
/// - On the Rust *implementation* side, use `CxxResult<T>` which does error
///   formatting in a more preferred way
/// - On the C++ side, use our custom `CXX_TRY` API which converts the C++ exception
///   into a GError.  In the future, we might try a hard switch to C++ exceptions
///   instead, but at the moment having two is problematic, so we prefer `GError`.
///
#[cxx::bridge(namespace = "rpmostreecxx")]
#[allow(clippy::needless_lifetimes)]
#[allow(unsafe_op_in_unsafe_fn)]
pub mod ffi {
    // Types that are defined by gtk-rs generated bindings that
    // we want to pass across the cxx-rs boundary.  For more
    // information, see cxx_bridge_gobject.rs.
    extern "C++" {
        include!("src/libpriv/rpmostree-cxxrs-prelude.h");

        type OstreeDeployment = crate::FFIOstreeDeployment;
        #[allow(dead_code)]
        type OstreeRepo = crate::FFIOstreeRepo;
        type OstreeRepoTransactionStats = crate::FFIOstreeRepoTransactionStats;
        type OstreeSysroot = crate::FFIOstreeSysroot;
        type GObject = crate::FFIGObject;
        type GCancellable = crate::FFIGCancellable;
        type GDBusConnection = crate::FFIGDBusConnection;
        type GFileInfo = crate::FFIGFileInfo;
        type GVariant = crate::FFIGVariant;
        type GVariantDict = crate::FFIGVariantDict;
        type GKeyFile = crate::FFIGKeyFile;

        #[namespace = "dnfcxx"]
        type DnfPackage = libdnf_sys::DnfPackage;
        #[namespace = "dnfcxx"]
        type DnfRepo = libdnf_sys::DnfRepo;
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
        fn is_bare_split_xattrs() -> Result<bool>;
        fn is_http_arg(arg: &str) -> bool;
        fn is_ostree_container() -> Result<bool>;
        fn is_rpm_arg(arg: &str) -> bool;
        fn client_start_daemon() -> Result<()>;
        fn client_handle_fd_argument(arg: &str, arch: &str) -> Result<Vec<i32>>;
        fn client_render_download_progress(progress: Pin<&mut GVariant>) -> String;
        fn running_in_container() -> bool;
    }

    #[derive(Debug)]
    pub(crate) enum BubblewrapMutability {
        Immutable,
        RoFiles,
        MutateFreely,
    }

    // bubblewrap.rs
    extern "Rust" {
        type Bubblewrap;

        fn bubblewrap_selftest() -> Result<()>;
        fn bubblewrap_run_sync(
            rootfs_dfd: i32,
            args: &Vec<String>,
            capture_stdout: bool,
            unified_core: bool,
        ) -> Result<Vec<u8>>;

        fn bubblewrap_new(rootfs_fd: i32) -> Result<Box<Bubblewrap>>;
        fn bubblewrap_new_with_mutability(
            rootfs_fd: i32,
            mutability: BubblewrapMutability,
        ) -> Result<Box<Bubblewrap>>;
        fn get_rootfs_fd(&self) -> i32;

        fn append_bwrap_arg(&mut self, arg: &str);
        fn append_child_arg(&mut self, arg: &str);
        fn setenv(&mut self, k: &str, v: &str);
        fn take_fd(&mut self, source_fd: i32, target_fd: i32);
        fn set_inherit_stdin(&mut self);
        fn take_stdin_fd(&mut self, source_fd: i32);
        fn take_stdout_fd(&mut self, source_fd: i32);
        fn take_stderr_fd(&mut self, source_fd: i32);
        fn take_stdout_and_stderr_fd(&mut self, source_fd: i32);

        fn bind_read(&mut self, src: &str, dest: &str);
        fn bind_readwrite(&mut self, src: &str, dest: &str);
        fn setup_compat_var(&mut self) -> Result<()>;

        fn run(&mut self, cancellable: Pin<&mut GCancellable>) -> Result<()>;
    }

    // builtins/apply_live.rs
    extern "Rust" {
        fn applylive_entrypoint(args: &Vec<String>) -> Result<()>;
        fn applylive_finish(sysroot: Pin<&mut OstreeSysroot>) -> Result<()>;
    }

    // builtins/compose/
    extern "Rust" {
        fn composeutil_legacy_prep_dev_and_run(rootfs_dfd: i32) -> Result<()>;
        fn print_ostree_txn_stats(stats: Pin<&mut OstreeRepoTransactionStats>);
        fn write_commit_id(target_path: &str, revision: &str) -> Result<()>;
    }

    // cliwrap.rs
    extern "Rust" {
        fn cliwrap_write_wrappers(rootfs: i32) -> Result<()>;
        fn cliwrap_destdir() -> String;
    }

    /// `ContainerImageState` is currently identical to ostree-rs-ext's `LayeredImageState` struct, because
    /// cxx.rs currently requires types used as extern Rust types to be defined by the same crate
    /// that contains the bridge using them, so we redefine an `ContainerImport` struct here.
    #[derive(Debug)]
    pub(crate) struct ContainerImageState {
        pub base_commit: String,
        pub merge_commit: String,
        pub is_layered: bool,
        pub image_digest: String,
    }

    // sysroot_upgrade.rs
    extern "Rust" {
        fn pull_container(
            repo: Pin<&mut OstreeRepo>,
            cancellable: Pin<&mut GCancellable>,
            imgref: &str,
        ) -> Result<Box<ContainerImageState>>;
        fn query_container_image(
            repo: Pin<&mut OstreeRepo>,
            imgref: &str,
        ) -> Result<Box<ContainerImageState>>;
    }

    // core.rs
    extern "Rust" {
        type TempEtcGuard;
        type FilesystemScriptPrep;

        fn prepare_tempetc_guard(rootfs: i32) -> Result<Box<TempEtcGuard>>;
        fn undo(self: &TempEtcGuard) -> Result<()>;

        fn prepare_filesystem_script_prep(rootfs: i32) -> Result<Box<FilesystemScriptPrep>>;
        fn undo(self: &FilesystemScriptPrep) -> Result<()>;

        fn run_depmod(rootfs_dfd: i32, kver: &str, unified_core: bool) -> Result<()>;

        fn log_treefile(tf: &Treefile);

        fn is_container_image_reference(refspec: &str) -> bool;
    }

    // composepost.rs
    extern "Rust" {
        fn compose_prepare_rootfs(
            src_rootfs_dfd: i32,
            dest_rootfs_dfd: i32,
            treefile: &mut Treefile,
        ) -> Result<()>;
        fn composepost_nsswitch_altfiles(rootfs_dfd: i32) -> Result<()>;
        fn compose_postprocess(
            rootfs_dfd: i32,
            treefile: &mut Treefile,
            next_version: &str,
            unified_core: bool,
        ) -> Result<()>;
        fn compose_postprocess_final(rootfs_dfd: i32) -> Result<()>;
        fn convert_var_to_tmpfiles_d(
            rootfs_dfd: i32,
            cancellable: Pin<&mut GCancellable>,
        ) -> Result<()>;
        fn rootfs_prepare_links(rootfs_dfd: i32) -> Result<()>;
        fn workaround_selinux_cross_labeling(
            rootfs_dfd: i32,
            cancellable: Pin<&mut GCancellable>,
        ) -> Result<()>;
        fn prepare_rpmdb_base_location(
            rootfs_dfd: i32,
            cancellable: Pin<&mut GCancellable>,
        ) -> Result<()>;
        fn compose_postprocess_rpm_macro(rootfs_dfd: i32) -> Result<()>;
        fn rewrite_rpmdb_for_target(rootfs_dfd: i32, normalize: bool) -> Result<()>;
        fn directory_size(dfd: i32, mut cancellable: Pin<&mut GCancellable>) -> Result<u64>;
    }

    // A grab-bag of metadata from the deployment's ostree commit
    // around layering/derivation
    #[derive(Debug, Default)]
    struct DeploymentLayeredMeta {
        is_layered: bool,
        base_commit: String,
        clientlayer_version: u32,
    }

    #[derive(Debug)]
    enum PackageOverrideSourceKind {
        Repo,
    }

    #[derive(Debug)]
    struct PackageOverrideSource {
        kind: PackageOverrideSourceKind,
        name: String,
    }

    #[derive(Debug)]
    enum ParsedRevisionKind {
        Version,
        Checksum,
    }

    #[derive(Debug)]
    struct ParsedRevision {
        kind: ParsedRevisionKind,
        value: String,
    }

    // daemon.rs
    extern "Rust" {
        fn daemon_sanitycheck_environment(sysroot: Pin<&mut OstreeSysroot>) -> Result<()>;
        fn deployment_generate_id(deployment: Pin<&mut OstreeDeployment>) -> String;
        fn deployment_populate_variant(
            mut sysroot: Pin<&mut OstreeSysroot>,
            mut deployment: Pin<&mut OstreeDeployment>,
            mut dict: Pin<&mut GVariantDict>,
        ) -> Result<()>;
        fn generate_baselayer_refs(
            mut sysroot: Pin<&mut OstreeSysroot>,
            mut repo: Pin<&mut OstreeRepo>,
            cancellable: Pin<&mut GCancellable>,
        ) -> Result<()>;
        fn variant_add_remote_status(
            mut repo: Pin<&mut OstreeRepo>,
            refspec: &str,
            base_checksum: &str,
            mut dict: Pin<&mut GVariantDict>,
        ) -> Result<()>;
        fn deployment_layeredmeta_from_commit(
            mut deployment: Pin<&mut OstreeDeployment>,
            mut commit: Pin<&mut GVariant>,
        ) -> Result<DeploymentLayeredMeta>;
        fn deployment_layeredmeta_load(
            mut repo: Pin<&mut OstreeRepo>,
            mut deployment: Pin<&mut OstreeDeployment>,
        ) -> Result<DeploymentLayeredMeta>;
        fn parse_override_source(source: &str) -> Result<PackageOverrideSource>;
        fn parse_revision(source: &str) -> Result<ParsedRevision>;
    }

    // failpoint_bridge.rs
    extern "Rust" {
        fn failpoint(p: &str) -> Result<()>;
    }

    // importer.rs
    extern "Rust" {
        fn importer_compose_filter(
            path: &str,
            mut file_info: Pin<&mut GFileInfo>,
            skip_extraneous: bool,
        ) -> Result<bool>;
        fn tmpfiles_translate(
            abs_path: &str,
            mut file_info: Pin<&mut GFileInfo>,
            username: &str,
            groupname: &str,
        ) -> Result<String>;
        fn tweak_imported_file_info(mut file_info: Pin<&mut GFileInfo>, ro_executables: bool);
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

    // modularity.rs
    extern "Rust" {
        fn modularity_entrypoint(args: &Vec<String>) -> Result<()>;
    }

    // tokio_ffi.rs
    extern "Rust" {
        type TokioHandle;
        type TokioEnterGuard<'a>;

        fn tokio_handle_get() -> Box<TokioHandle>;
        unsafe fn enter(self: &TokioHandle) -> Box<TokioEnterGuard>;
    }

    // scripts.rs
    extern "Rust" {
        fn script_is_ignored(pkg: &str, script: &str) -> bool;
    }

    // testutils.rs
    extern "Rust" {
        fn testutils_entrypoint(argv: Vec<String>) -> Result<()>;
        fn maybe_shell_quote(input: &str) -> String;
    }

    // treefile.rs
    #[derive(Debug)]
    enum RepoMetadataTarget {
        Inline,
        Detached,
        Disabled,
    }

    extern "Rust" {
        type Treefile;

        fn treefile_new(filename: &str, basearch: &str, workdir: i32) -> Result<Box<Treefile>>;
        fn treefile_new_empty() -> Result<Box<Treefile>>;
        fn treefile_new_from_string(buf: &str, client: bool) -> Result<Box<Treefile>>;
        fn treefile_new_compose(
            filename: &str,
            basearch: &str,
            workdir: i32,
        ) -> Result<Box<Treefile>>;
        fn treefile_new_client(filename: &str, basearch: &str) -> Result<Box<Treefile>>;
        fn treefile_new_client_from_etc(basearch: &str) -> Result<Box<Treefile>>;
        fn treefile_delete_client_etc() -> Result<u32>;

        fn get_workdir(&self) -> i32;
        fn get_passwd_fd(&mut self) -> i32;
        fn get_group_fd(&mut self) -> i32;
        fn get_json_string(&self) -> String;
        fn get_ostree_layers(&self) -> Vec<String>;
        fn get_ostree_override_layers(&self) -> Vec<String>;
        fn get_all_ostree_layers(&self) -> Vec<String>;
        fn get_repos(&self) -> Vec<String>;
        fn get_packages(&self) -> Vec<String>;
        fn set_packages(&mut self, packages: &Vec<String>);
        fn get_packages_local(&self) -> Vec<String>;
        fn get_packages_local_fileoverride(&self) -> Vec<String>;
        fn get_packages_override_replace_local(&self) -> Vec<String>;
        fn get_packages_override_replace_local_rpms(&self) -> Vec<String>;
        fn set_packages_override_replace_local_rpms(&mut self, packages: &Vec<String>);
        fn get_packages_override_remove(&self) -> Vec<String>;
        fn set_packages_override_remove(&mut self, packages: &Vec<String>);
        fn get_modules_enable(&self) -> Vec<String>;
        fn get_modules_install(&self) -> Vec<String>;
        fn get_exclude_packages(&self) -> Vec<String>;
        fn get_platform_module(&self) -> String;
        fn get_install_langs(&self) -> Vec<String>;
        fn format_install_langs_macro(&self) -> String;
        fn get_lockfile_repos(&self) -> Vec<String>;
        fn get_ref(&self) -> &str;
        fn get_cliwrap(&self) -> bool;
        fn get_container_cmd(&self) -> Vec<String>;
        fn get_readonly_executables(&self) -> bool;
        fn get_documentation(&self) -> bool;
        fn get_recommends(&self) -> bool;
        fn get_selinux(&self) -> bool;
        fn get_releasever(&self) -> String;
        fn get_repo_metadata_target(&self) -> RepoMetadataTarget;
        fn rpmdb_backend_is_target(&self) -> bool;
        fn should_normalize_rpmdb(&self) -> bool;
        fn get_files_remove_regex(&self, package: &str) -> Vec<String>;
        fn get_checksum(&self, repo: Pin<&mut OstreeRepo>) -> Result<String>;
        fn get_ostree_ref(&self) -> String;
        fn get_repo_packages(&self) -> &[RepoPackage];
        fn clear_repo_packages(&mut self);
        fn prettyprint_json_stdout(&self);
        fn print_deprecation_warnings(&self);
        fn print_experimental_notices(&self);
        fn sanitycheck_externals(&self) -> Result<()>;
        fn validate_for_container(&self) -> Result<()>;
    }

    // treefile.rs (split out from above to make &self nice to use)
    extern "Rust" {
        type RepoPackage;

        fn get_repo(&self) -> &str;
        fn get_packages(&self) -> &[String];
    }

    // utils.rs
    extern "Rust" {
        fn varsubstitute(s: &str, vars: &Vec<StringMapping>) -> Result<String>;
        fn get_features() -> Vec<String>;
        fn get_rpm_basearch() -> String;
        fn sealed_memfd(description: &str, content: &[u8]) -> Result<i32>;
        fn running_in_systemd() -> bool;
        fn calculate_advisories_diff(
            repo: Pin<&mut OstreeRepo>,
            checksum_from: &str,
            checksum_to: &str,
        ) -> Result<*mut GVariant>;
    }

    #[derive(Debug, Default)]
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
        fn transaction_apply_live(
            sysroot: Pin<&mut OstreeSysroot>,
            target: Pin<&mut GVariant>,
        ) -> Result<()>;
    }

    // passwd.rs
    extern "Rust" {
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
        fn check_passwd_group_entries(
            mut ffi_repo: Pin<&mut OstreeRepo>,
            rootfs_dfd: i32,
            treefile: &mut Treefile,
            previous_rev: &str,
        ) -> Result<()>;

        fn passwddb_open(rootfs: i32) -> Result<Box<PasswdDB>>;
        type PasswdDB;
        fn lookup_user(self: &PasswdDB, uid: u32) -> Result<String>;
        fn lookup_group(self: &PasswdDB, gid: u32) -> Result<String>;

        fn new_passwd_entries() -> Box<PasswdEntries>;
        type PasswdEntries;
        fn add_group_content(self: &mut PasswdEntries, rootfs: i32, path: &str) -> Result<()>;
        fn add_passwd_content(self: &mut PasswdEntries, rootfs: i32, path: &str) -> Result<()>;
        fn contains_group(self: &PasswdEntries, user: &str) -> bool;
        fn contains_user(self: &PasswdEntries, user: &str) -> bool;
        fn lookup_user_id(self: &PasswdEntries, user: &str) -> Result<u32>;
        fn lookup_group_id(self: &PasswdEntries, group: &str) -> Result<u32>;
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
        fn generate_treefile(&self, src: &Treefile) -> Result<Box<Treefile>>;
    }

    #[derive(Debug)]
    struct LockedPackage {
        name: String,
        evr: String,
        arch: String,
        digest: String,
    }

    // lockfile.rs
    extern "Rust" {
        type LockfileConfig;

        fn lockfile_read(filenames: &Vec<String>) -> Result<Box<LockfileConfig>>;
        fn lockfile_write(
            filename: &str,
            packages: Pin<&mut CxxGObjectArray>,
            rpmmd_repos: Pin<&mut CxxGObjectArray>,
        ) -> Result<()>;

        fn get_locked_packages(&self) -> Result<Vec<LockedPackage>>;
        fn get_locked_src_packages(&self) -> Result<Vec<LockedPackage>>;
    }

    // origin.rs
    extern "Rust" {
        fn origin_to_treefile(kf: Pin<&mut GKeyFile>) -> Result<Box<Treefile>>;
        fn origin_validate_roundtrip(mut kf: Pin<&mut GKeyFile>);
    }

    // rpmutils.rs
    extern "Rust" {
        fn cache_branch_to_nevra(nevra: &str) -> String;
    }

    unsafe extern "C++" {
        include!("rpmostree-cxxrsutil.hpp");
        #[allow(missing_debug_implementations)]
        type CxxGObjectArray;
        fn length(self: Pin<&mut CxxGObjectArray>) -> u32;
        fn get(self: Pin<&mut CxxGObjectArray>, i: u32) -> &mut GObject;
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
        fn rpmostree_main(args: &[&str]) -> Result<i32>;
        fn rpmostree_process_global_teardown();
        fn c_unit_tests() -> Result<()>;
    }

    unsafe extern "C++" {
        include!("rpmostree-clientlib.h");
        fn client_require_root() -> Result<()>;
        #[allow(missing_debug_implementations)]
        type ClientConnection;
        fn new_client_connection() -> Result<UniquePtr<ClientConnection>>;
        fn get_connection<'a>(self: Pin<&'a mut ClientConnection>) -> Pin<&'a mut GDBusConnection>;
        fn transaction_connect_progress_sync(&self, address: &str) -> Result<()>;
    }

    unsafe extern "C++" {
        include!("rpmostree-diff.hpp");
        #[allow(missing_debug_implementations)]
        type RPMDiff;
        fn n_removed(&self) -> i32;
        fn n_added(&self) -> i32;
        fn n_modified(&self) -> i32;
        fn rpmdb_diff(
            repo: Pin<&mut OstreeRepo>,
            src: &CxxString,
            dest: &CxxString,
            allow_noent: bool,
        ) -> Result<UniquePtr<RPMDiff>>;

        fn print(&self);
    }

    // https://cxx.rs/shared.html#extern-enums
    #[derive(Debug)]
    enum RpmOstreeDiffPrintFormat {
        RPMOSTREE_DIFF_PRINT_FORMAT_SUMMARY,
        RPMOSTREE_DIFF_PRINT_FORMAT_FULL_ALIGNED,
        RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE,
    }

    unsafe extern "C++" {
        include!("rpmostree-libbuiltin.h");
        include!("rpmostree-util.h");
        #[allow(missing_debug_implementations)]
        type RpmOstreeDiffPrintFormat;
        /// # Safety: ensure @cancellable is a valid pointer
        unsafe fn print_treepkg_diff_from_sysroot_path(
            sysroot_path: &str,
            format: RpmOstreeDiffPrintFormat,
            max_key_len: u32,
            cancellable: *mut GCancellable,
        );
    }

    unsafe extern "C++" {
        include!("rpmostree-output.h");
        #[allow(missing_debug_implementations)]
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
        fn nevra_to_cache_branch(nevra: &CxxString) -> Result<String>;
        fn get_repodata_chksum_repr(pkg: &mut DnfPackage) -> Result<String>;
    }

    // rpmostree-package-variants.h
    unsafe extern "C++" {
        include!("rpmostree-package-variants.h");
        fn package_variant_list_for_commit(
            repo: Pin<&mut OstreeRepo>,
            rev: &str,
            cancellable: Pin<&mut GCancellable>,
        ) -> Result<*mut GVariant>;
    }
}

mod builtins;
pub(crate) use crate::builtins::apply_live::*;
pub(crate) use crate::builtins::compose::commit::*;
pub(crate) use crate::builtins::compose::*;
mod bwrap;
pub(crate) use bwrap::*;
mod client;
pub(crate) use client::*;
pub mod cliwrap;
pub mod container;
pub use cliwrap::*;
mod composepost;
pub mod countme;
pub(crate) use composepost::*;
mod core;
use crate::core::*;
mod capstdext;
mod daemon;
mod dirdiff;
pub mod failpoint_bridge;
pub(crate) use daemon::*;
use failpoint_bridge::*;
mod extensions;
pub(crate) use extensions::*;
#[cfg(feature = "fedora-integration")]
mod fedora_integration;
mod history;
pub use self::history::*;
mod importer;
pub(crate) use importer::*;
mod initramfs;
pub(crate) use self::initramfs::*;
mod isolation;
mod journal;
pub(crate) use self::journal::*;
mod lockfile;
pub(crate) use self::lockfile::*;
mod live;
pub(crate) use self::live::*;
pub mod modularity;
pub(crate) use self::modularity::*;
mod nameservice;
mod normalization;
mod origin;
pub(crate) use self::origin::*;
mod passwd;
use passwd::*;
mod console_progress;
pub(crate) use self::console_progress::*;
mod progress;
mod tokio_ffi;
pub(crate) use self::tokio_ffi::*;
mod scripts;
pub(crate) use self::scripts::*;
mod sysroot_upgrade;
pub(crate) use crate::sysroot_upgrade::*;
mod rpmutils;
pub(crate) use self::rpmutils::*;
mod testutils;
pub(crate) use self::testutils::*;
mod treefile;
pub use self::treefile::*;
mod utils;
pub use self::utils::*;
mod variant_utils;
