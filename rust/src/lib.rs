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
#![deny(clippy::dbg_macro)]
#![deny(clippy::todo)]
#![allow(clippy::ptr_arg)]

// pub(crate) utilities
mod cxxrsutil;
mod ffiutil;
pub(crate) mod ffiwrappers;
pub(crate) use cxxrsutil::*;

/// APIs defined here are automatically bridged between Rust and C++ using https://cxx.rs/
///
/// # Regenerating
///
/// After you change APIs in here, you must run `make -f Makefile.bindings`
/// to regenerate the C++ bridge side.
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
        type OstreeSePolicy = crate::FFIOstreeSePolicy;
        type GObject = crate::FFIGObject;
        type GCancellable = crate::FFIGCancellable;
        type GDBusConnection = crate::FFIGDBusConnection;
        type GFileInfo = crate::FFIGFileInfo;
        type GVariant = crate::FFIGVariant;
        type GVariantDict = crate::FFIGVariantDict;
        type GKeyFile = crate::FFIGKeyFile;

        #[namespace = "dnfcxx"]
        type FFIDnfPackage = libdnf_sys::FFIDnfPackage;
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

    /// Classify the running system.
    #[derive(Clone, Debug)]
    enum SystemHostType {
        OstreeContainer,
        OstreeHost,
        Unknown,
    }

    // client.rs
    extern "Rust" {
        fn is_bare_split_xattrs() -> Result<bool>;
        fn is_http_arg(arg: &str) -> bool;
        fn is_ostree_container() -> Result<bool>;
        fn get_system_host_type() -> Result<SystemHostType>;
        fn require_system_host_type(t: SystemHostType) -> Result<()>;
        fn is_rpm_arg(arg: &str) -> bool;
        fn client_start_daemon() -> Result<()>;
        fn client_handle_fd_argument(arg: &str, arch: &str, is_replace: bool) -> Result<Vec<i32>>;
        fn client_render_download_progress(progress: &GVariant) -> String;
        fn running_in_container() -> bool;
        fn confirm() -> Result<bool>;
        fn confirm_or_abort() -> Result<()>;
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

        fn run(&mut self, cancellable: &GCancellable) -> Result<()>;
    }

    // builtins/apply_live.rs
    extern "Rust" {
        fn applylive_entrypoint(args: &Vec<String>) -> Result<()>;
        fn applylive_finish(sysroot: &OstreeSysroot) -> Result<()>;
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
        fn cliwrap_write_some_wrappers(rootfs: i32, args: &Vec<String>) -> Result<()>;
        fn cliwrap_destdir() -> String;
    }

    // container.rs
    extern "Rust" {
        fn container_encapsulate(args: Vec<String>) -> Result<()>;
        fn deploy_from_self_entrypoint(args: Vec<String>) -> Result<()>;
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
        pub version: String,
    }

    // sysroot_upgrade.rs
    extern "Rust" {
        fn pull_container(
            repo: &OstreeRepo,
            cancellable: &GCancellable,
            imgref: &str,
        ) -> Result<Box<ContainerImageState>>;
        fn container_prune(repo: &OstreeRepo, cancellable: &GCancellable) -> Result<()>;
        fn query_container_image_commit(
            repo: &OstreeRepo,
            c: &str,
        ) -> Result<Box<ContainerImageState>>;
        fn purge_refspec(repo: &OstreeRepo, refspec: &str) -> Result<()>;
    }

    // core.rs
    #[derive(Debug, PartialEq, Eq)]
    enum RefspecType {
        Ostree,
        Checksum,
        Container,
    }

    // core.rs
    extern "Rust" {
        type TempEtcGuard;
        type FilesystemScriptPrep;

        fn prepare_tempetc_guard(rootfs: i32) -> Result<Box<TempEtcGuard>>;
        fn undo(self: &TempEtcGuard) -> Result<()>;

        fn prepare_filesystem_script_prep(rootfs: i32) -> Result<Box<FilesystemScriptPrep>>;
        fn undo(self: &mut FilesystemScriptPrep) -> Result<()>;

        fn run_depmod(rootfs_dfd: i32, kver: &str, unified_core: bool) -> Result<()>;

        fn log_treefile(tf: &Treefile);

        fn is_container_image_reference(refspec: &str) -> bool;
        fn refspec_classify(refspec: &str) -> RefspecType;

        fn verify_kernel_hmac(rootfs: i32, moddir: &str) -> Result<()>;

        fn stage_container_rpms(rpms: Vec<String>) -> Result<Vec<String>>;
        fn stage_container_rpm_raw_fds(fds: Vec<i32>) -> Result<Vec<String>>;

        fn commit_has_matching_sepolicy(commit: &GVariant, policy: &OstreeSePolicy)
            -> Result<bool>;

        fn get_header_variant(repo: &OstreeRepo, cachebranch: &str) -> Result<*mut GVariant>;
    }

    // compose.rs
    extern "Rust" {
        fn compose_image(args: Vec<String>) -> Result<()>;

        fn configure_build_repo_from_target(
            build_repo: &OstreeRepo,
            target_repo: &OstreeRepo,
        ) -> Result<()>;
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
        fn convert_var_to_tmpfiles_d(rootfs_dfd: i32, cancellable: &GCancellable) -> Result<()>;
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
        fn postprocess_cleanup_rpmdb(rootfs_dfd: i32) -> Result<()>;
        fn rewrite_rpmdb_for_target(rootfs_dfd: i32, normalize: bool) -> Result<()>;
        fn directory_size(dfd: i32, cancellable: &GCancellable) -> Result<u64>;
    }

    // container.cxx
    unsafe extern "C++" {
        include!("rpmostree-container.hpp");
        fn container_rebuild(treefile: &str) -> Result<()>;
    }

    // deployment_utils.rs
    extern "Rust" {
        fn deployment_for_id(
            sysroot: Pin<&mut OstreeSysroot>,
            deploy_id: &str,
        ) -> Result<*mut OstreeDeployment>;
        fn deployment_checksum_for_id(
            sysroot: Pin<&mut OstreeSysroot>,
            deploy_id: &str,
        ) -> Result<String>;
        fn deployment_get_base(
            sysroot: Pin<&mut OstreeSysroot>,
            opt_deploy_id: &str,
            opt_os_name: &str,
        ) -> Result<*mut OstreeDeployment>;

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
    struct OverrideReplacementSource {
        kind: OverrideReplacementType,
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
        fn daemon_main(debug: bool) -> Result<()>;
        fn daemon_terminate();
        fn daemon_sanitycheck_environment(sysroot: &OstreeSysroot) -> Result<()>;
        fn deployment_generate_id(deployment: &OstreeDeployment) -> String;
        fn deployment_populate_variant(
            sysroot: &OstreeSysroot,
            deployment: &OstreeDeployment,
            dict: &GVariantDict,
        ) -> Result<()>;
        fn generate_baselayer_refs(
            sysroot: &OstreeSysroot,
            repo: &OstreeRepo,
            cancellable: &GCancellable,
        ) -> Result<()>;
        fn variant_add_remote_status(
            repo: &OstreeRepo,
            refspec: &str,
            base_checksum: &str,
            dict: &GVariantDict,
        ) -> Result<()>;
        fn deployment_layeredmeta_from_commit(
            deployment: &OstreeDeployment,
            commit: &GVariant,
        ) -> Result<DeploymentLayeredMeta>;
        fn deployment_layeredmeta_load(
            repo: &OstreeRepo,
            deployment: &OstreeDeployment,
        ) -> Result<DeploymentLayeredMeta>;
        fn parse_override_source(source: &str) -> Result<OverrideReplacementSource>;
        fn parse_revision(source: &str) -> Result<ParsedRevision>;
        fn generate_object_path(base: &str, next_segment: &str) -> Result<String>;
    }

    // failpoints.rs
    extern "Rust" {
        fn failpoint(p: &str) -> Result<()>;
    }

    // importer.rs
    extern "Rust" {
        type RpmImporterFlags;
        fn rpm_importer_flags_new_empty() -> Box<RpmImporterFlags>;
        fn is_ima_enabled(self: &RpmImporterFlags) -> bool;

        type RpmImporter;
        fn rpm_importer_new(
            pkg_name: &str,
            ostree_branch: &str,
            flags: &RpmImporterFlags,
        ) -> Result<Box<RpmImporter>>;
        fn handle_translate_pathname(self: &mut RpmImporter, path: &str) -> String;
        fn ostree_branch(self: &RpmImporter) -> String;
        fn pkg_name(self: &RpmImporter) -> String;
        fn doc_files_are_filtered(self: &RpmImporter) -> bool;
        fn doc_files_insert(self: &mut RpmImporter, path: &str);
        fn doc_files_contains(self: &RpmImporter, path: &str) -> bool;
        fn rpmfi_overrides_insert(self: &mut RpmImporter, path: &str, index: u64);
        fn rpmfi_overrides_contains(self: &RpmImporter, path: &str) -> bool;
        fn rpmfi_overrides_get(self: &RpmImporter, path: &str) -> u64;
        fn is_ima_enabled(self: &RpmImporter) -> bool;
        fn tweak_imported_file_info(self: &RpmImporter, mut file_info: &GFileInfo);
        fn is_file_filtered(self: &RpmImporter, path: &str, file_info: &GFileInfo) -> Result<bool>;
        fn translate_to_tmpfiles_entry(
            self: &mut RpmImporter,
            abs_path: &str,
            file_info: &GFileInfo,
            username: &str,
            groupname: &str,
        ) -> Result<()>;
        fn has_tmpfiles_entries(self: &RpmImporter) -> bool;
        fn serialize_tmpfiles_content(self: &RpmImporter) -> String;

        fn tmpfiles_translate(
            abs_path: &str,
            file_info: &GFileInfo,
            username: &str,
            groupname: &str,
        ) -> Result<String>;
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

    #[derive(Debug, PartialEq, Eq)]
    struct Refspec {
        kind: RefspecType,
        refspec: String,
    }

    // This is an awkward almost duplicate of the types in treefile.rs, but cxx.rs-compatible.
    #[derive(Debug)]
    enum OverrideReplacementType {
        Repo,
    }

    #[derive(Debug, PartialEq, Eq)]
    struct OverrideReplacement {
        from: String,
        from_kind: OverrideReplacementType,
        packages: Vec<String>,
    }

    extern "Rust" {
        type Treefile;

        fn treefile_new(filename: &str, basearch: &str) -> Result<Box<Treefile>>;
        fn treefile_new_empty() -> Result<Box<Treefile>>;
        fn treefile_new_from_string(buf: &str, client: bool) -> Result<Box<Treefile>>;
        fn treefile_new_compose(filename: &str, basearch: &str) -> Result<Box<Treefile>>;
        fn treefile_new_client(filename: &str, basearch: &str) -> Result<Box<Treefile>>;
        fn treefile_new_client_from_etc(basearch: &str) -> Result<Box<Treefile>>;
        fn treefile_delete_client_etc() -> Result<u32>;

        fn get_workdir(&self) -> &str;
        fn get_passwd_fd(&mut self) -> i32;
        fn get_group_fd(&mut self) -> i32;
        fn get_json_string(&self) -> String;
        fn get_ostree_layers(&self) -> Vec<String>;
        fn get_ostree_override_layers(&self) -> Vec<String>;
        fn get_all_ostree_layers(&self) -> Vec<String>;
        fn get_repos(&self) -> Vec<String>;
        fn get_packages(&self) -> Vec<String>;
        fn require_automatic_version_prefix(&self) -> Result<String>;
        fn add_packages(&mut self, packages: Vec<String>, allow_existing: bool) -> Result<bool>;
        fn has_packages(&self) -> bool;
        fn get_local_packages(&self) -> Vec<String>;
        fn add_local_packages(
            &mut self,
            packages: Vec<String>,
            allow_existing: bool,
        ) -> Result<bool>;
        fn get_local_fileoverride_packages(&self) -> Vec<String>;
        fn add_local_fileoverride_packages(
            &mut self,
            packages: Vec<String>,
            allow_existing: bool,
        ) -> Result<bool>;
        fn remove_packages(&mut self, packages: Vec<String>, allow_noent: bool) -> Result<bool>;
        fn get_packages_override_replace(&self) -> Vec<OverrideReplacement>;
        fn has_packages_override_replace(&self) -> bool;
        fn add_packages_override_replace(&mut self, replacement: OverrideReplacement) -> bool;
        fn remove_package_override_replace(&mut self, package: &str) -> bool;
        fn get_packages_override_replace_local(&self) -> Vec<String>;
        fn add_packages_override_replace_local(&mut self, packages: Vec<String>) -> Result<()>;
        fn remove_package_override_replace_local(&mut self, package: &str) -> bool;
        fn get_packages_override_remove(&self) -> Vec<String>;
        fn add_packages_override_remove(&mut self, packages: Vec<String>) -> Result<()>;
        fn remove_package_override_remove(&mut self, package: &str) -> bool;
        fn has_packages_override_remove_name(&self, name: &str) -> bool;
        fn remove_all_overrides(&mut self) -> bool;
        fn get_modules_enable(&self) -> Vec<String>;
        fn has_modules_enable(&self) -> bool;
        fn get_modules_install(&self) -> Vec<String>;
        fn add_modules(&mut self, modules: Vec<String>, enable_only: bool) -> bool;
        fn remove_modules(&mut self, modules: Vec<String>, enable_only: bool) -> bool;
        fn remove_all_packages(&mut self) -> bool;
        fn get_exclude_packages(&self) -> Vec<String>;
        fn get_platform_module(&self) -> String;
        fn get_install_langs(&self) -> Vec<String>;
        fn format_install_langs_macro(&self) -> String;
        fn get_lockfile_repos(&self) -> Vec<String>;
        fn get_ref(&self) -> &str;
        fn get_cliwrap(&self) -> bool;
        fn get_cliwrap_binaries(&self) -> Vec<String>;
        fn set_cliwrap(&mut self, enabled: bool);
        fn get_container_cmd(&self) -> Vec<String>;
        fn get_readonly_executables(&self) -> bool;
        fn get_documentation(&self) -> bool;
        fn get_recommends(&self) -> bool;
        fn get_selinux(&self) -> bool;
        fn get_gpg_key(&self) -> String;
        fn get_automatic_version_suffix(&self) -> String;
        fn get_container(&self) -> bool;
        fn get_machineid_compat(&self) -> bool;
        fn get_etc_group_members(&self) -> Vec<String>;
        fn get_boot_location_is_modules(&self) -> bool;
        fn get_ima(&self) -> bool;
        fn get_releasever(&self) -> String;
        fn get_repo_metadata_target(&self) -> RepoMetadataTarget;
        fn rpmdb_backend_is_target(&self) -> bool;
        fn should_normalize_rpmdb(&self) -> bool;
        fn get_files_remove_regex(&self, package: &str) -> Vec<String>;
        fn get_checksum(&self, repo: &OstreeRepo) -> Result<String>;
        fn get_ostree_ref(&self) -> String;
        fn get_repo_packages(&self) -> &[RepoPackage];
        fn clear_repo_packages(&mut self);
        fn prettyprint_json_stdout(&self);
        fn print_deprecation_warnings(&self);
        fn print_experimental_notices(&self);
        fn sanitycheck_externals(&self) -> Result<()>;
        fn importer_flags(&self, pkg_name: &str) -> Box<RpmImporterFlags>;
        fn write_repovars(&self, workdir_dfd_raw: i32) -> Result<String>;
        fn set_releasever(&mut self, releasever: &str) -> Result<()>;
        fn enable_repo(&mut self, repo: &str) -> Result<()>;
        fn disable_repo(&mut self, repo: &str) -> Result<()>;
        // these functions are more related to derivation
        fn validate_for_container(&self) -> Result<()>;
        fn get_base_refspec(&self) -> Refspec;
        fn rebase(
            &mut self,
            new_refspec: &str,
            custom_origin_url: &str,
            custom_origin_description: &str,
        );
        fn get_origin_custom_url(&self) -> String;
        fn get_origin_custom_description(&self) -> String;
        fn get_override_commit(&self) -> String;
        fn set_override_commit(&mut self, checksum: &str);
        fn get_initramfs_etc_files(&self) -> Vec<String>;
        fn has_initramfs_etc_files(&self) -> bool;
        fn initramfs_etc_files_track(&mut self, files: Vec<String>) -> bool;
        fn initramfs_etc_files_untrack(&mut self, files: Vec<String>) -> bool;
        fn initramfs_etc_files_untrack_all(&mut self) -> bool;
        fn get_initramfs_regenerate(&self) -> bool;
        fn get_initramfs_args(&self) -> Vec<String>;
        fn set_initramfs_regenerate(&mut self, enabled: bool, args: Vec<String>);
        fn get_unconfigured_state(&self) -> String;
        fn may_require_local_assembly(&self) -> bool;
        fn has_any_packages(&self) -> bool;
        fn merge_treefile(&mut self, treefile: &str) -> Result<bool>;
    }

    // treefile.rs (split out from above to make &self nice to use)
    extern "Rust" {
        type RepoPackage;

        fn get_repo(&self) -> &str;
        fn get_packages(&self) -> Vec<String>;
    }

    // utils.rs
    extern "Rust" {
        fn varsubstitute(s: &str, vars: &Vec<StringMapping>) -> Result<String>;
        fn get_features() -> Vec<String>;
        fn get_rpm_basearch() -> String;
        fn sealed_memfd(description: &str, content: &[u8]) -> Result<i32>;
        fn running_in_systemd() -> bool;
        fn calculate_advisories_diff(
            repo: &OstreeRepo,
            checksum_from: &str,
            checksum_to: &str,
        ) -> Result<*mut GVariant>;
        fn translate_path_for_ostree(path: &str) -> String;
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
            sysroot: &OstreeSysroot,
            deployment: &OstreeDeployment,
        ) -> Result<LiveApplyState>;
        fn has_live_apply_state(
            sysroot: &OstreeSysroot,
            deployment: &OstreeDeployment,
        ) -> Result<bool>;
        fn applylive_sync_ref(sysroot: &OstreeSysroot) -> Result<()>;
        fn transaction_apply_live(sysroot: &OstreeSysroot, target: &GVariant) -> Result<()>;
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
            repo: &OstreeRepo,
            previous_checksum: &str,
            unified_core: bool,
        ) -> Result<()>;
        fn dir_contains_uid(dirfd: i32, id: u32) -> Result<bool>;
        fn dir_contains_gid(dirfd: i32, id: u32) -> Result<bool>;
        fn check_passwd_group_entries(
            mut ffi_repo: &OstreeRepo,
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
    }

    // origin.rs
    extern "Rust" {
        fn origin_to_treefile(kf: &GKeyFile) -> Result<Box<Treefile>>;
        fn treefile_to_origin(tf: &Treefile) -> Result<*mut GKeyFile>;
        fn origin_validate_roundtrip(kf: &GKeyFile);
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
        fn testutil_validate_cxxrs_passthrough(repo: &OstreeRepo) -> i32;
    }

    unsafe extern "C++" {
        include!("rpmostreemain.h");
        fn early_main();
        fn rpmostree_main(args: &[&str]) -> Result<i32>;
        fn rpmostree_process_global_teardown();
        fn c_unit_tests() -> Result<()>;
    }

    unsafe extern "C++" {
        include!("rpmostreed-daemon.hpp");
        fn daemon_init_inner(debug: bool) -> Result<()>;
        fn daemon_main_inner() -> Result<()>;
    }

    unsafe extern "C++" {
        include!("rpmostree-clientlib.h");
        fn client_require_root() -> Result<()>;
        #[allow(missing_debug_implementations)]
        type ClientConnection;
        fn new_client_connection() -> Result<UniquePtr<ClientConnection>>;
        fn get_connection<'a>(self: Pin<&'a mut ClientConnection>) -> &'a GDBusConnection;
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
            repo: &OstreeRepo,
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
        #[allow(missing_debug_implementations)]
        type RpmTs;
        #[allow(missing_debug_implementations)]
        type PackageMeta;

        // Currently only used in unit tests
        #[allow(dead_code)]
        fn nevra_to_cache_branch(nevra: &CxxString) -> Result<String>;
        fn get_repodata_chksum_repr(pkg: &mut FFIDnfPackage) -> Result<String>;
        fn rpmts_for_commit(repo: &OstreeRepo, rev: &str) -> Result<UniquePtr<RpmTs>>;
        fn rpmdb_package_name_list(dfd: i32, path: String) -> Result<Vec<String>>;

        // Methods on RpmTs
        fn packages_providing_file(self: &RpmTs, path: &str) -> Result<Vec<String>>;
        fn package_meta(self: &RpmTs, name: &str) -> Result<UniquePtr<PackageMeta>>;

        // Methods on PackageMeta
        fn size(self: &PackageMeta) -> u64;
        fn buildtime(self: &PackageMeta) -> u64;
        fn changelogs(self: &PackageMeta) -> Vec<u64>;
        fn src_pkg(self: &PackageMeta) -> &CxxString;
    }

    // rpmostree-package-variants.h
    unsafe extern "C++" {
        include!("rpmostree-package-variants.h");
        fn package_variant_list_for_commit(
            repo: &OstreeRepo,
            rev: &str,
            cancellable: &GCancellable,
        ) -> Result<*mut GVariant>;
    }
}

pub mod builtins;
pub(crate) use crate::builtins::apply_live::*;
pub(crate) use crate::builtins::compose::commit::*;
pub(crate) use crate::builtins::compose::*;
mod bwrap;
pub(crate) use bwrap::*;
pub mod client;
pub(crate) use client::*;
pub mod cliwrap;
pub mod container;
pub use cliwrap::*;
pub(crate) use container::*;
mod compose;
pub(crate) use compose::*;
mod composepost;
pub mod countme;
pub(crate) use composepost::*;
mod core;
use crate::core::*;
mod capstdext;
mod daemon;
pub(crate) use daemon::*;
mod deployment_utils;
pub(crate) use deployment_utils::*;
mod dirdiff;
pub mod failpoints;
use failpoints::*;
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
pub mod utils;
pub use self::utils::*;
mod variant_utils;
