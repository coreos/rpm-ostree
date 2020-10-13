//! Core implementation logic for "livefs" which applies
//! changes to an overlayfs on top of `/usr` in the booted
//! deployment.
/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use anyhow::{bail, Context, Result};
use nix::sys::statvfs;
use openat_ext::OpenatDirExt;
use ostree::DeploymentUnlockedState;
use rayon::prelude::*;
use serde_derive::{Deserialize, Serialize};
use std::borrow::Cow;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};
use std::process::Command;

/// The directory where ostree stores transient per-deployment state.
/// This is currently semi-private to ostree; we should add an API to
/// access it.
const OSTREE_RUNSTATE_DIR: &str = "/run/ostree/deployment-state";
/// Filename we use for serialized state, stored in the above directory.
const LIVEFS_STATE_NAME: &str = "rpmostree-livefs-state.json";

/// The model for livefs state.
#[derive(Debug, Default, Clone, Serialize, Deserialize)]
struct LiveFsState {
    /// The OSTree commit that the running root filesystem is using,
    /// as distinct from the one it was booted with.
    commit: Option<String>,
    /// Set when a livefs operation is in progress; if the process
    /// is interrupted, some files from this commit may exist
    /// on disk but in an incomplete state.
    inprogress_commit: Option<String>,
}

/// Get the transient state directory for a deployment; TODO
/// upstream this into libostree.
fn get_runstate_dir(deploy: &ostree::Deployment) -> PathBuf {
    format!(
        "{}/{}.{}",
        OSTREE_RUNSTATE_DIR,
        deploy.get_csum().expect("csum"),
        deploy.get_deployserial()
    )
    .into()
}

/// Get the livefs state
fn get_livefs_state(deploy: &ostree::Deployment) -> Result<Option<LiveFsState>> {
    let root = openat::Dir::open("/")?;
    if let Some(f) = root.open_file_optional(&get_runstate_dir(deploy).join(LIVEFS_STATE_NAME))? {
        let s: LiveFsState = serde_json::from_reader(std::io::BufReader::new(f))?;
        Ok(Some(s))
    } else {
        Ok(None)
    }
}

/// Write new livefs state
fn write_livefs_state(deploy: &ostree::Deployment, state: &LiveFsState) -> Result<()> {
    let rundir = get_runstate_dir(deploy);
    let rundir = openat::Dir::open(&rundir)?;
    rundir.write_file_with(LIVEFS_STATE_NAME, 0o644, |w| -> Result<_> {
        Ok(serde_json::to_writer(w, state)?)
    })?;
    Ok(())
}

/// Get the relative parent directory of a path
fn relpath_dir(p: &Path) -> Result<&Path> {
    Ok(p.strip_prefix("/")?.parent().expect("parent"))
}

/// Return a path buffer we can provide to libostree for checkout
fn subpath(diff: &crate::ostree_diff::FileTreeDiff, p: &Path) -> Option<PathBuf> {
    if let Some(ref d) = diff.subdir {
        let p = p.strip_prefix("/").expect("prefix");
        Some(Path::new(d).join(p))
    } else {
        Some(p.to_path_buf())
    }
}

/// Given a diff, apply it to the target directory, which should be a checkout of the source commit.
fn apply_diff(
    repo: &ostree::Repo,
    diff: &crate::ostree_diff::FileTreeDiff,
    commit: &str,
    destdir: &openat::Dir,
) -> Result<()> {
    if !diff.changed_dirs.is_empty() {
        anyhow::bail!("Changed directories are not supported yet");
    }
    let cancellable = gio::NONE_CANCELLABLE;
    // This applies to all added/changed content, we just
    // overwrite `subpath` in each run.
    let mut opts = ostree::RepoCheckoutAtOptions {
        overwrite_mode: ostree::RepoCheckoutOverwriteMode::UnionFiles,
        force_copy: true,
        ..Default::default()
    };
    // Check out new directories and files
    for d in diff.added_dirs.iter().map(Path::new) {
        opts.subpath = subpath(diff, &d);
        let t = d.strip_prefix("/")?;
        repo.checkout_at(Some(&opts), destdir.as_raw_fd(), t, commit, cancellable)
            .with_context(|| format!("Checking out added dir {:?}", d))?;
    }
    for d in diff.added_files.iter().map(Path::new) {
        opts.subpath = subpath(diff, &d);
        repo.checkout_at(
            Some(&opts),
            destdir.as_raw_fd(),
            relpath_dir(d)?,
            commit,
            cancellable,
        )
        .with_context(|| format!("Checking out added file {:?}", d))?;
    }
    // Changed files in existing directories
    for d in diff.changed_files.iter().map(Path::new) {
        opts.subpath = subpath(diff, &d);
        repo.checkout_at(
            Some(&opts),
            destdir.as_raw_fd(),
            relpath_dir(d)?,
            commit,
            cancellable,
        )
        .with_context(|| format!("Checking out changed file {:?}", d))?;
    }
    assert!(diff.changed_dirs.is_empty());

    // Finally clean up removed directories and files together.  We use
    // rayon here just because we can.
    diff.removed_files
        .par_iter()
        .chain(diff.removed_dirs.par_iter())
        .try_for_each(|d| -> Result<()> {
            let d = d.strip_prefix("/").expect("prefix");
            destdir
                .remove_all(d)
                .with_context(|| format!("Failed to remove {:?}", d))?;
            Ok(())
        })?;

    Ok(())
}

/// Special handling for `/etc` - we currently just add new default files/directories.
/// We don't try to delete anything yet, because doing so could mess up the actual
/// `/etc` merge on reboot between the real deployment.  Much of the logic here
/// is similar to what libostree core does for `/etc` on upgrades.  If we ever
/// push livefs down into libostree, this logic could be shared.
fn update_etc(
    repo: &ostree::Repo,
    diff: &crate::ostree_diff::FileTreeDiff,
    sepolicy: &ostree::SePolicy,
    commit: &str,
    destdir: &openat::Dir,
) -> Result<()> {
    let expected_subpath = "/usr";
    // We stripped both /usr and /etc, we need to readd them both
    // for the checkout.
    fn filtermap_paths(s: &String) -> Option<(Option<PathBuf>, &Path)> {
        s.strip_prefix("/etc").map(|p| {
            let p = Path::new(p).strip_prefix("/").expect("prefix");
            (Some(Path::new("/usr/etc").join(p)), p)
        })
    }
    // For some reason in Rust the `parent()` of `foo` is just the empty string `""`; we
    // need it to be the self-link `.` path.
    fn canonicalized_parent(p: &Path) -> &Path {
        match p.parent() {
            Some(p) if p.as_os_str().is_empty() => Path::new("."),
            Some(p) => p,
            None => Path::new("."),
        }
    }

    // The generic apply_diff() above in theory could work anywhere.
    // But this code is only designed for /etc.
    assert_eq!(diff.subdir.as_ref().expect("subpath"), expected_subpath);
    if !diff.changed_dirs.is_empty() {
        anyhow::bail!("Changed directories are not supported yet");
    }

    let cancellable = gio::NONE_CANCELLABLE;
    // This applies to all added/changed content, we just
    // overwrite `subpath` in each run.
    let mut opts = ostree::RepoCheckoutAtOptions {
        overwrite_mode: ostree::RepoCheckoutOverwriteMode::UnionFiles,
        force_copy: true,
        ..Default::default()
    };
    // The labels for /etc and /usr/etc may differ; ensure that we label
    // the files with the /etc target, even though we're checking out
    // from /usr/etc.  This is the same as what libostree does.
    if sepolicy.get_name().is_some() {
        opts.sepolicy = Some(sepolicy.clone());
    }
    // Added directories and files
    for (subpath, target) in diff.added_dirs.iter().filter_map(filtermap_paths) {
        opts.subpath = subpath;
        repo.checkout_at(
            Some(&opts),
            destdir.as_raw_fd(),
            target,
            commit,
            cancellable,
        )
        .with_context(|| format!("Checking out added /etc dir {:?}", (&opts.subpath, target)))?;
    }
    for (subpath, target) in diff.added_files.iter().filter_map(filtermap_paths) {
        opts.subpath = subpath;
        repo.checkout_at(
            Some(&opts),
            destdir.as_raw_fd(),
            canonicalized_parent(target),
            commit,
            cancellable,
        )
        .with_context(|| format!("Checking out added /etc file {:?}", (&opts.subpath, target)))?;
    }
    // Now changed files
    for (subpath, target) in diff.changed_files.iter().filter_map(filtermap_paths) {
        opts.subpath = subpath;
        repo.checkout_at(
            Some(&opts),
            destdir.as_raw_fd(),
            canonicalized_parent(target),
            commit,
            cancellable,
        )
        .with_context(|| {
            format!(
                "Checking out changed /etc file {:?}",
                (&opts.subpath, target)
            )
        })?;
    }
    assert!(diff.changed_dirs.is_empty());

    // And finally clean up removed files and directories.
    diff.removed_files
        .par_iter()
        .chain(diff.removed_dirs.par_iter())
        .filter_map(filtermap_paths)
        .try_for_each(|(_, target)| -> Result<()> {
            destdir
                .remove_all(target)
                .with_context(|| format!("Failed to remove {:?}", target))?;
            Ok(())
        })?;

    Ok(())
}

// Our main process uses MountFlags=slave set up by systemd;
// this is what allows us to e.g. remount /sysroot writable
// just inside our mount namespace.  However, in this case
// we actually need to escape our mount namespace and affect
// the "main" mount namespace so that other processes will
// see the overlayfs.
fn unlock_transient(sysroot: &ostree::Sysroot) -> Result<()> {
    // Temporarily drop the lock
    sysroot.unlock();
    let status = Command::new("systemd-run")
        .args(&[
            "-u",
            "rpm-ostree-unlock",
            "--wait",
            "--",
            "ostree",
            "admin",
            "unlock",
            "--transient",
        ])
        .status();
    sysroot.lock()?;
    let status = status?;
    if !status.success() {
        bail!("Failed to unlock --transient");
    }
    Ok(())
}

/// Run `systemd-tmpfiles` via `systemd-run` so we escape our mount namespace.
/// This allows our `ProtectHome=` in the unit file to work.
fn rerun_tmpfiles() -> Result<()> {
    for prefix in &["/run", "/var"] {
        let status = Command::new("systemd-run")
            .args(&[
                "-u",
                "rpm-ostree-tmpfiles",
                "--wait",
                "--",
                "systemd-tmpfiles",
                "--create",
                "--prefix",
                prefix,
            ])
            .status()?;
        if !status.success() {
            bail!("Failed to invoke systemd-tmpfiles");
        }
    }
    Ok(())
}

/// Implementation of `rpm-ostree ex livefs`.
fn livefs(sysroot: &ostree::Sysroot, target: Option<&str>) -> Result<()> {
    let repo = sysroot.repo().expect("repo");
    let repo = &repo;

    let booted = if let Some(b) = sysroot.get_booted_deployment() {
        b
    } else {
        bail!("Not booted into an OSTree system")
    };
    let osname = booted.get_osname().expect("osname");
    let booted_commit = booted.get_csum().expect("csum");
    let booted_commit = booted_commit.as_str();

    let target_commit = if let Some(t) = target {
        Cow::Borrowed(t)
    } else {
        match crate::ostree_utils::sysroot_query_deployments_for(sysroot, osname.as_str()) {
            (Some(pending), _) => {
                let pending_commit = pending.get_csum().expect("csum");
                let pending_commit = pending_commit.as_str();
                Cow::Owned(pending_commit.to_string())
            }
            (None, _) => {
                anyhow::bail!("No target commit specified and no pending deployment");
            }
        }
    };

    let state = get_livefs_state(&booted)?;
    if state.is_none() {
        match booted.get_unlocked() {
            DeploymentUnlockedState::None => {
                unlock_transient(sysroot)?;
            }
            DeploymentUnlockedState::Transient | DeploymentUnlockedState::Development => {}
            s => {
                bail!("livefs is incompatible with unlock state: {}", s);
            }
        };
    } else {
        match booted.get_unlocked() {
            DeploymentUnlockedState::Transient | DeploymentUnlockedState::Development => {}
            s => {
                bail!("deployment not unlocked, is in state: {}", s);
            }
        };
    }
    // In the transient mode, remount writable - this affects just the rpm-ostreed
    // mount namespace.  In the future it'd be nicer to run transactions as subprocesses
    // so we don't lift the writable protection for the main rpm-ostree process.
    if statvfs::statvfs("/usr")?
        .flags()
        .contains(statvfs::FsFlags::ST_RDONLY)
    {
        use nix::mount::MsFlags;
        let none: Option<&str> = None;
        nix::mount::mount(
            none,
            "/usr",
            none,
            MsFlags::MS_REMOUNT | MsFlags::MS_SILENT,
            none,
        )?;
    }

    if let Some(ref state) = state {
        if let Some(ref inprogress) = state.inprogress_commit {
            if inprogress.as_str() != target_commit {
                bail!(
                    "Previously interrupted while targeting commit {}, cannot change target to {}",
                    inprogress,
                    target_commit
                )
            }
        }
    }

    let source_commit = state
        .as_ref()
        .map(|s| s.commit.as_deref())
        .flatten()
        .unwrap_or(booted_commit);
    let diff = crate::ostree_diff::diff(repo, source_commit, &target_commit, Some("/usr"))
        .context("Failed computing diff")?;

    let mut state = state.unwrap_or_default();

    let rootfs_dfd = openat::Dir::open("/")?;
    let sepolicy = ostree::SePolicy::new_at(rootfs_dfd.as_raw_fd(), gio::NONE_CANCELLABLE)?;

    // Record that we're targeting this commit
    state.inprogress_commit = Some(target_commit.to_string());
    write_livefs_state(&booted, &state)?;

    // The heart of things: updating the overlayfs on /usr
    apply_diff(repo, &diff, &target_commit, &openat::Dir::open("/usr")?)?;

    // The other important bits are /etc and /var
    update_etc(
        repo,
        &diff,
        &sepolicy,
        &target_commit,
        &openat::Dir::open("/etc")?,
    )?;
    rerun_tmpfiles()?;

    // Success! Update the recorded state.
    state.commit = Some(target_commit.to_string());
    state.inprogress_commit = None;
    write_livefs_state(&booted, &state)?;

    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_subpath() {
        let d = crate::ostree_diff::FileTreeDiff {
            subdir: Some("/usr".to_string()),
            ..Default::default()
        };
        let s = subpath(&d, Path::new("/foo"));
        assert_eq!(s.as_ref().map(|s| s.as_path()), Some(Path::new("/usr/foo")));
    }
}

mod ffi {
    use super::*;
    use glib;
    use glib::translate::*;
    use glib::GString;
    use glib_sys;
    use libc;

    use crate::ffiutil::*;

    #[no_mangle]
    pub extern "C" fn ror_livefs_get_state(
        sysroot: *mut ostree_sys::OstreeSysroot,
        deployment: *mut ostree_sys::OstreeDeployment,
        out_inprogress: *mut *mut libc::c_char,
        out_replaced: *mut *mut libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let _sysroot: ostree::Sysroot = unsafe { from_glib_none(sysroot) };
        let deployment: ostree::Deployment = unsafe { from_glib_none(deployment) };
        match get_livefs_state(&deployment) {
            Ok(Some(state)) => {
                unsafe {
                    if let Some(c) = state.inprogress_commit {
                        *out_inprogress = c.to_glib_full();
                    }
                    if let Some(c) = state.commit {
                        *out_replaced = c.to_glib_full();
                    }
                }
                1
            }
            Ok(None) => 1,
            Err(ref e) => {
                error_to_glib(e, gerror);
                0
            }
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_transaction_livefs(
        sysroot: *mut ostree_sys::OstreeSysroot,
        target: *const libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let sysroot: ostree::Sysroot = unsafe { from_glib_none(sysroot) };
        let target: Borrowed<Option<GString>> = unsafe { from_glib_borrow(target) };
        // The reference hole goes deep
        let target = target.as_ref().as_ref().map(|s| s.as_str());
        int_glib_error(livefs(&sysroot, target), gerror)
    }
}
pub use self::ffi::*;
