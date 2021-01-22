//! Core implementation logic for "apply-live" which applies
//! changes to an overlayfs on top of `/usr` in the booted
//! deployment.
/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use crate::cxxrsutil::*;
use crate::ffi::LiveApplyState;
use anyhow::{anyhow, bail, Context, Result};
use nix::sys::statvfs;
use openat_ext::OpenatDirExt;
use ostree::DeploymentUnlockedState;
use rayon::prelude::*;
use serde_derive::{Deserialize, Serialize};
use std::borrow::Cow;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::process::Command;

/// The directory where ostree stores transient per-deployment state.
/// This is currently semi-private to ostree; we should add an API to
/// access it.
const OSTREE_RUNSTATE_DIR: &str = "/run/ostree/deployment-state";
/// Filename we use for serialized state, stored in the above directory.
const LIVE_STATE_NAME: &str = "rpmostree-live-state.json";

/// The model for live state.  This representation is
/// just used "on disk" right now because
/// TODO(cxx-rs) doesn't support Option<T>
#[derive(Debug, Default, Clone, Eq, PartialEq, Serialize, Deserialize)]
struct LiveApplyStateSerialized {
    /// The OSTree commit that the running root filesystem is using,
    /// as distinct from the one it was booted with.
    commit: Option<String>,
    /// Set when an apply-live operation is in progress; if the process
    /// is interrupted, some files from this commit may exist
    /// on disk but in an incomplete state.
    inprogress: Option<String>,
}

impl From<&LiveApplyStateSerialized> for LiveApplyState {
    fn from(s: &LiveApplyStateSerialized) -> LiveApplyState {
        LiveApplyState {
            inprogress: s.inprogress.clone().unwrap_or_default(),
            commit: s.commit.clone().unwrap_or_default(),
        }
    }
}

impl From<&LiveApplyState> for LiveApplyStateSerialized {
    fn from(s: &LiveApplyState) -> LiveApplyStateSerialized {
        LiveApplyStateSerialized {
            inprogress: Some(s.inprogress.clone()).filter(|s| !s.is_empty()),
            commit: Some(s.commit.clone()).filter(|s| !s.is_empty()),
        }
    }
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

/// Get the live state
fn get_live_state(deploy: &ostree::Deployment) -> Result<Option<LiveApplyState>> {
    let root = openat::Dir::open("/")?;
    if let Some(f) = root.open_file_optional(&get_runstate_dir(deploy).join(LIVE_STATE_NAME))? {
        let s: LiveApplyStateSerialized = serde_json::from_reader(std::io::BufReader::new(f))?;
        let s = &s;
        Ok(Some(s.into()))
    } else {
        Ok(None)
    }
}

/// Write new livefs state
fn write_live_state(deploy: &ostree::Deployment, state: &LiveApplyState) -> Result<()> {
    let rundir = get_runstate_dir(deploy);
    let rundir = openat::Dir::open(&rundir)?;
    let state: LiveApplyStateSerialized = state.into();
    rundir.write_file_with(LIVE_STATE_NAME, 0o644, |w| -> Result<_> {
        Ok(serde_json::to_writer(w, &state)?)
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
/// push apply-live down into libostree, this logic could be shared.
fn update_etc(
    repo: &ostree::Repo,
    diff: &crate::ostree_diff::FileTreeDiff,
    config_diff: &crate::dirdiff::Diff,
    sepolicy: &ostree::SePolicy,
    commit: &str,
    destdir: &openat::Dir,
) -> Result<()> {
    let expected_subpath = "/usr";
    // We stripped both /usr and /etc, we need to readd them both
    // for the checkout.
    let filtermap_paths = |s: &String| -> Option<(Option<PathBuf>, PathBuf)> {
        s.strip_prefix("/etc/")
            .filter(|p| !config_diff.contains(p))
            .map(|p| {
                let p = Path::new(p);
                (Some(Path::new("/usr/etc").join(p)), p.into())
            })
    };
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
            &target,
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
            canonicalized_parent(&target),
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
            canonicalized_parent(&target),
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
                .remove_all(&target)
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

/// Implementation of `rpm-ostree ex apply-live`.
pub(crate) fn transaction_apply_live(
    mut sysroot: Pin<&mut crate::ffi::OstreeSysroot>,
    target: &str,
) -> CxxResult<()> {
    let sysroot = &sysroot.gobj_wrap();
    let target = if !target.is_empty() {
        Some(target)
    } else {
        None
    };
    let repo = &sysroot.repo().expect("repo");

    let booted = if let Some(b) = sysroot.get_booted_deployment() {
        b
    } else {
        return Err(anyhow!("Not booted into an OSTree system").into());
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
                return Err(anyhow!("No target commit specified and no pending deployment").into());
            }
        }
    };

    let state = get_live_state(&booted)?;
    if state.is_none() {
        match booted.get_unlocked() {
            DeploymentUnlockedState::None => {
                unlock_transient(sysroot)?;
            }
            DeploymentUnlockedState::Transient | DeploymentUnlockedState::Development => {}
            s => {
                return Err(anyhow!("apply-live is incompatible with unlock state: {}", s).into());
            }
        };
    } else {
        match booted.get_unlocked() {
            DeploymentUnlockedState::Transient | DeploymentUnlockedState::Development => {}
            s => {
                return Err(anyhow!("deployment not unlocked, is in state: {}", s).into());
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
        if !state.inprogress.is_empty() && state.inprogress.as_str() != target_commit {
            return Err(anyhow::anyhow!(
                "Previously interrupted while targeting commit {}, cannot change target to {}",
                state.inprogress,
                target_commit
            )
            .into());
        }
    }

    let source_commit = state
        .as_ref()
        .map(|s| s.commit.as_str())
        .filter(|s| !s.is_empty())
        .unwrap_or(booted_commit);
    let diff = crate::ostree_diff::diff(repo, source_commit, &target_commit, Some("/usr"))
        .context("Failed computing diff")?;
    println!("Computed /usr diff: {}", &diff);

    let mut state = state.unwrap_or_default();

    let rootfs_dfd = openat::Dir::open("/")?;
    let sepolicy = ostree::SePolicy::new_at(rootfs_dfd.as_raw_fd(), gio::NONE_CANCELLABLE)?;

    // Record that we're targeting this commit
    state.inprogress = target_commit.to_string();
    write_live_state(&booted, &state)?;

    // Gather the current diff of /etc - we need to avoid changing
    // any files which are locally modified.
    let config_diff = {
        let usretc = &rootfs_dfd.sub_dir("usr/etc")?;
        let etc = &rootfs_dfd.sub_dir("etc")?;
        crate::dirdiff::diff(usretc, etc)?
    };
    println!("Computed /etc diff: {}", &config_diff);

    // The heart of things: updating the overlayfs on /usr
    apply_diff(repo, &diff, &target_commit, &openat::Dir::open("/usr")?)?;

    // The other important bits are /etc and /var
    update_etc(
        repo,
        &diff,
        &config_diff,
        &sepolicy,
        &target_commit,
        &openat::Dir::open("/etc")?,
    )?;
    rerun_tmpfiles()?;

    // Success! Update the recorded state.
    state.commit = target_commit.to_string();
    state.inprogress = "".to_string();
    write_live_state(&booted, &state)?;

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

    #[test]
    fn test_repr() {
        let s: LiveApplyStateSerialized = Default::default();
        let b: LiveApplyState = (&s).into();
        assert_eq!(b.commit, "");
        assert_eq!(b.inprogress, "");
        let rs: LiveApplyStateSerialized = (&b).into();
        assert_eq!(rs, s);
        let s = LiveApplyStateSerialized {
            commit: Some("42".to_string()),
            inprogress: None,
        };
        let b: LiveApplyState = (&s).into();
        assert_eq!(b.commit, "42");
        assert_eq!(b.inprogress, "");
        let rs: LiveApplyStateSerialized = (&b).into();
        assert_eq!(rs, s);
    }
}

pub(crate) fn get_live_apply_state(
    mut _sysroot: Pin<&mut crate::ffi::OstreeSysroot>,
    mut deployment: Pin<&mut crate::ffi::OstreeDeployment>,
) -> CxxResult<LiveApplyState> {
    let deployment = deployment.gobj_wrap();
    if let Some(state) = get_live_state(&deployment)? {
        Ok(state)
    } else {
        Ok(Default::default())
    }
}

pub(crate) fn has_live_apply_state(
    sysroot: Pin<&mut crate::ffi::OstreeSysroot>,
    deployment: Pin<&mut crate::ffi::OstreeDeployment>,
) -> CxxResult<bool> {
    let state = get_live_apply_state(sysroot, deployment)?;
    Ok(!(state.commit.is_empty() && state.inprogress.is_empty()))
}
