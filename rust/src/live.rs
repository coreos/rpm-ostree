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
use crate::isolation;
use crate::progress::progress_task;
use anyhow::{anyhow, Context, Result};
use cap_std::fs::Dir;
use cap_std_ext::cap_std;
use cap_std_ext::dirext::CapStdExtDirExt;
use fn_error_context::context;
use nix::sys::statvfs;
use ostree::DeploymentUnlockedState;
use ostree_ext::diff::FileTreeDiff;
use ostree_ext::{gio, glib, ostree};
use rayon::prelude::*;
use std::borrow::Cow;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};

/// GVariant `s`: Choose a specific commit
pub(crate) const OPT_TARGET: &str = "target";
/// GVariant `b`: Enable changing or removing packages(+files).
pub(crate) const OPT_REPLACE: &str = "replace";

/// The directory where ostree stores transient per-deployment state.
/// This is currently semi-private to ostree; we should add an API to
/// access it.
const OSTREE_RUNSTATE_DIR: &str = "ostree/deployment-state";
/// Stamp file used to signal deployment was live-applied, stored in the above directory
const LIVE_STATE_NAME: &str = "rpmostree-is-live.stamp";
/// OSTree ref that follows the live state
const LIVE_REF: &str = "rpmostree/live-apply";
/// OSTree ref that will be set to the commit we are currently
/// updating to; if the process is interrupted, we can then
/// more reliably resynchronize.
const LIVE_REF_INPROGRESS: &str = "rpmostree/live-apply-inprogress";

/// Get the transient state directory for a deployment; TODO
/// upstream this into libostree.
fn get_runstate_dir(deploy: &ostree::Deployment) -> PathBuf {
    format!(
        "{}/{}.{}",
        OSTREE_RUNSTATE_DIR,
        deploy.csum(),
        deploy.deployserial()
    )
    .into()
}

/// Get the live state
pub(crate) fn get_live_state(
    repo: &ostree::Repo,
    deploy: &ostree::Deployment,
) -> Result<Option<LiveApplyState>> {
    let run = Dir::open_ambient_dir("/run", cap_std::ambient_authority())?;
    if !run.try_exists(&get_runstate_dir(deploy).join(LIVE_STATE_NAME))? {
        return Ok(None);
    }
    let live_commit = repo.resolve_rev(LIVE_REF, true)?;
    let inprogress_commit = repo.resolve_rev(LIVE_REF_INPROGRESS, true)?;
    Ok(Some(LiveApplyState {
        commit: live_commit.map(|s| s.to_string()).unwrap_or_default(),
        inprogress: inprogress_commit.map(|s| s.to_string()).unwrap_or_default(),
    }))
}

/// Write new livefs state
fn write_live_state(
    repo: &ostree::Repo,
    deploy: &ostree::Deployment,
    state: &LiveApplyState,
) -> Result<()> {
    let run = Dir::open_ambient_dir("/run", cap_std::ambient_authority())?;
    let rundir = if let Some(d) = run.open_dir_optional(&get_runstate_dir(deploy))? {
        d
    } else {
        return Ok(());
    };

    let found_live_stamp = rundir.exists(LIVE_STATE_NAME);

    let commit = Some(state.commit.as_str()).filter(|s| !s.is_empty());
    repo.set_ref_immediate(None, LIVE_REF, commit, gio::Cancellable::NONE)?;
    let inprogress_commit = Some(state.inprogress.as_str()).filter(|s| !s.is_empty());
    repo.set_ref_immediate(
        None,
        LIVE_REF_INPROGRESS,
        inprogress_commit,
        gio::Cancellable::NONE,
    )?;

    // Ensure the stamp file exists
    if !found_live_stamp && commit.or(inprogress_commit).is_some() {
        rundir.atomic_write(LIVE_STATE_NAME, b"")?;
    }

    Ok(())
}

/// Get the relative parent directory of a path
fn relpath_dir(p: &Path) -> Result<&Path> {
    Ok(p.strip_prefix("/")?.parent().expect("parent"))
}

/// Return a path buffer we can provide to libostree for checkout
fn subpath(diff: &FileTreeDiff, p: &Path) -> Option<PathBuf> {
    if let Some(ref d) = diff.subdir {
        let p = p.strip_prefix("/").expect("prefix");
        Some(Path::new(d).join(p))
    } else {
        Some(p.to_path_buf())
    }
}

/// Given a diff, apply it to the target directory, which should be a checkout of the source commit.
fn apply_diff(repo: &ostree::Repo, diff: &FileTreeDiff, commit: &str, destdir: &Dir) -> Result<()> {
    if !diff.changed_dirs.is_empty() {
        anyhow::bail!("Changed directories are not supported yet");
    }
    let cancellable = gio::Cancellable::NONE;
    // This applies to all added/changed content, we just
    // overwrite `subpath` in each run.
    let mut opts = ostree::RepoCheckoutAtOptions {
        overwrite_mode: ostree::RepoCheckoutOverwriteMode::UnionFiles,
        force_copy: true,
        ..Default::default()
    };
    // Check out new directories and files
    for d in diff.added_dirs.iter().map(Path::new) {
        opts.subpath = subpath(diff, d);
        let t = d.strip_prefix("/")?;
        repo.checkout_at(Some(&opts), destdir.as_raw_fd(), t, commit, cancellable)
            .with_context(|| format!("Checking out added dir {:?}", d))?;
    }
    for d in diff.added_files.iter().map(Path::new) {
        opts.subpath = subpath(diff, d);
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
        opts.subpath = subpath(diff, d);
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
            let d = d.strip_prefix('/').expect("prefix");
            destdir
                .remove_all_optional(d)
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
    diff: &FileTreeDiff,
    config_diff: &crate::dirdiff::Diff,
    sepolicy: &ostree::SePolicy,
    commit: &str,
    destdir: &Dir,
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

    let cancellable = gio::Cancellable::NONE;
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
    if sepolicy.name().len() > 0 {
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
                .remove_all_optional(&target)
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
#[context("Creating overlayfs")]
fn unlock_transient(sysroot: &ostree::Sysroot) -> Result<()> {
    // Temporarily drop the lock
    sysroot.unlock();
    isolation::run_systemd_worker_sync(&isolation::UnitConfig {
        name: Some("rpm-ostree-unlock"),
        properties: &[],
        exec_args: &["ostree", "admin", "unlock", "--transient"],
    })?;
    Ok(())
}

/// Run `systemd-tmpfiles` as a separate systemd unit to escape
/// our mount namespace.
/// This allows our `ProtectHome=` in the unit file to work
/// for example.  Longer term I'd like to protect even more of `/var`.
#[context("Running tmpfiles for /run and /var")]
fn rerun_tmpfiles() -> Result<()> {
    isolation::run_systemd_worker_sync(&isolation::UnitConfig {
        name: Some("rpm-ostree-tmpfiles"),
        properties: &[],
        exec_args: &[
            "systemd-tmpfiles",
            "--create",
            "--prefix=/run",
            "--prefix=/var",
        ],
    })
}

/// Implementation of `rpm-ostree apply-live`.
pub(crate) fn transaction_apply_live(
    sysroot: &crate::ffi::OstreeSysroot,
    options: &crate::ffi::GVariant,
) -> CxxResult<()> {
    let sysroot = &sysroot.glib_reborrow();
    let options = &options.glib_reborrow();
    let options = &glib::VariantDict::new(Some(options));
    let target = &options
        .lookup::<String>(OPT_TARGET)
        .map_err(anyhow::Error::msg)?;
    let allow_replacement: bool = options
        .lookup(OPT_REPLACE)
        .map_err(anyhow::Error::msg)?
        .unwrap_or_default();
    let repo = &sysroot.repo();

    let booted = sysroot.require_booted_deployment()?;
    let osname = booted.osname();
    let booted_commit = booted.csum();
    let booted_commit = booted_commit.as_str();

    let target_commit = if let Some(t) = target {
        Cow::Borrowed(t)
    } else {
        match sysroot.query_deployments_for(Some(osname.as_str())) {
            (Some(pending), _) => {
                let pending_commit = pending.csum();
                let pending_commit = pending_commit.as_str();
                Cow::Owned(pending_commit.to_string())
            }
            (None, _) => {
                return Err(anyhow!("No target commit specified and no pending deployment").into());
            }
        }
    };

    let state = get_live_state(repo, &booted)?;
    if state.is_none() {
        match booted.unlocked() {
            DeploymentUnlockedState::None => {
                unlock_transient(sysroot)?;
            }
            DeploymentUnlockedState::Transient | DeploymentUnlockedState::Development => {}
            s => {
                return Err(anyhow!("apply-live is incompatible with unlock state: {}", s).into());
            }
        };
    } else {
        match booted.unlocked() {
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
        if !state.inprogress.is_empty() && state.inprogress.as_str() != target_commit.as_str() {
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
    // Compute the filesystem-level diff
    let diff = ostree_ext::diff::diff(repo, source_commit, &target_commit, Some("/usr"))?;
    // And then the package-level diff
    let pkgdiff = {
        cxx::let_cxx_string!(from = source_commit);
        cxx::let_cxx_string!(to = &*target_commit);
        crate::ffi::rpmdb_diff(repo.reborrow_cxx(), &from, &to, false)
            .map_err(anyhow::Error::msg)?
    };
    if !allow_replacement {
        if pkgdiff.n_removed() > 0 {
            return Err(anyhow!(
                "packages would be removed: {}, enable replacement to override",
                pkgdiff.n_removed()
            )
            .into());
        }
        if pkgdiff.n_modified() > 0 {
            return Err(anyhow!(
                "packages would be changed: {}, enable replacement to override",
                pkgdiff.n_modified()
            )
            .into());
        }
    }

    println!("Computed /usr diff: {}", &diff);
    println!(
        "Computed pkg diff: {} added, {} changed, {} removed",
        pkgdiff.n_added(),
        pkgdiff.n_modified(),
        pkgdiff.n_removed()
    );

    let mut state = state.unwrap_or_default();

    let rootfs_dfd = Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
    let sepolicy = ostree::SePolicy::new_at(rootfs_dfd.as_raw_fd(), gio::Cancellable::NONE)?;

    // Record that we're targeting this commit
    state.inprogress = target_commit.to_string();
    write_live_state(repo, &booted, &state)?;

    // Gather the current diff of /etc - we need to avoid changing
    // any files which are locally modified.
    let config_diff = progress_task("Computing /etc diff to preserve", || -> Result<_> {
        let usretc = &rootfs_dfd.open_dir("usr/etc")?;
        let etc = &rootfs_dfd.open_dir("etc")?;
        crate::dirdiff::diff(usretc, etc)
    })?;
    println!("Computed /etc diff: {}", &config_diff);

    // The heart of things: updating the overlayfs on /usr
    let usr = &Dir::open_ambient_dir("/usr", cap_std::ambient_authority())?;
    progress_task("Updating /usr", || -> Result<_> {
        apply_diff(repo, &diff, &target_commit, usr)
    })?;

    // The other important bits are /etc and /var
    let etc = &Dir::open_ambient_dir("/etc", cap_std::ambient_authority())?;
    progress_task("Updating /etc", || -> Result<_> {
        update_etc(repo, &diff, &config_diff, &sepolicy, &target_commit, etc)
    })?;
    progress_task("Running systemd-tmpfiles for /run and /var", rerun_tmpfiles)?;

    // Success! Update the recorded state.
    state.commit = target_commit.to_string();
    state.inprogress = "".to_string();
    write_live_state(repo, &booted, &state)?;

    Ok(())
}

/// Writing a ref for the live-apply state can get out of sync
/// if we upgrade.  This prunes the ref if the booted deployment
/// doesn't have a live apply state in /run.
pub(crate) fn applylive_sync_ref(sysroot: &crate::ffi::OstreeSysroot) -> CxxResult<()> {
    let sysroot = sysroot.glib_reborrow();
    let repo = &sysroot.repo();
    let booted = if let Some(b) = sysroot.booted_deployment() {
        b
    } else {
        return Ok(());
    };
    if get_live_state(repo, &booted)?.is_some() {
        return Ok(());
    }

    // Set the live state to empty
    let state = Default::default();
    write_live_state(repo, &booted, &state).context("apply-live: failed to write state")?;
    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_subpath() {
        let d = FileTreeDiff {
            subdir: Some("/usr".to_string()),
            ..Default::default()
        };
        let s = subpath(&d, Path::new("/foo"));
        assert_eq!(s.as_deref(), Some(Path::new("/usr/foo")));
    }
}

pub(crate) fn get_live_apply_state(
    sysroot: &crate::ffi::OstreeSysroot,
    deployment: &crate::ffi::OstreeDeployment,
) -> CxxResult<LiveApplyState> {
    let sysroot = sysroot.glib_reborrow();
    let deployment = deployment.glib_reborrow();
    let repo = &sysroot.repo();
    if let Some(state) = get_live_state(repo, &deployment)? {
        Ok(state)
    } else {
        Ok(Default::default())
    }
}

pub(crate) fn has_live_apply_state(
    sysroot: &crate::ffi::OstreeSysroot,
    deployment: &crate::ffi::OstreeDeployment,
) -> CxxResult<bool> {
    let state = get_live_apply_state(sysroot, deployment)?;
    Ok(!(state.commit.is_empty() && state.inprogress.is_empty()))
}
