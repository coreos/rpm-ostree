//! APIs for interacting with `/etc/passwd` and `/etc/group`, including
//! handling the "nss-altfiles" split into `/usr/lib/{passwd,group}`.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffiutil;
use crate::nameservice;
use crate::normalization;
use crate::treefile::{CheckGroups, CheckPasswd, Treefile};
use anyhow::{anyhow, Context, Result};
use cap_std::fs::Dir;
use cap_std::fs::OpenOptions;
use cap_std_ext::cap_std;
use cap_std_ext::cap_std::fs::{
    DirBuilderExt, MetadataExt, OpenOptionsExt, Permissions, PermissionsExt,
};
use cap_std_ext::cap_tempfile;
use cap_std_ext::dirext::CapStdExtDirExt;
use fn_error_context::context;
use gio::prelude::*;
use nix::unistd::{Gid, Uid};
use once_cell::sync::Lazy;
use ostree_ext::{gio, ostree};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::os::unix::io::AsRawFd;
use std::path::Path;

const DEFAULT_MODE: u32 = 0o644;
static DEFAULT_PERMS: Lazy<Permissions> = Lazy::new(|| Permissions::from_mode(DEFAULT_MODE));
static PWGRP_SHADOW_FILES: &[&str] = &["shadow", "gshadow", "subuid", "subgid"];
static USRLIB_PWGRP_FILES: &[&str] = &["passwd", "group"];
// This stamp file signals the original fix which only changed the booted deployment
const SHADOW_MODE_FIXED_STAMP_OLD: &str = "etc/.rpm-ostree-shadow-mode-fixed.stamp";
// And this one is written by the newer logic that changes all deployments
const SHADOW_MODE_FIXED_STAMP: &str = "etc/.rpm-ostree-shadow-mode-fixed2.stamp";

// Lock/backup files that should not be in the base commit (TODO fix).
static PWGRP_LOCK_AND_BACKUP_FILES: &[&str] = &[
    ".pwd.lock",
    "passwd-",
    "group-",
    "shadow-",
    "gshadow-",
    "subuid-",
    "subgid-",
];

/// Prepare passwd content before layering RPMs.
///
/// We actually want RPM to inject to /usr/lib/passwd - we
/// accomplish this by temporarily renaming /usr/lib/passwd -> /usr/etc/passwd
/// (Which appears as /etc/passwd via our compatibility symlink in the bubblewrap
/// script runner). We also copy the merge deployment's /etc/passwd to
/// /usr/lib/passwd, so that %pre scripts are aware of newly added system users
/// not in the tree's /usr/lib/passwd (through nss-altfiles in the container).
#[context("Preparing passwd content")]
pub fn prepare_rpm_layering(rootfs_dfd: i32, merge_passwd_dir: &str) -> CxxResult<bool> {
    passwd_cleanup(rootfs_dfd)?;
    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs_dfd)? };
    let merge_passwd_dir: Option<&Path> = opt_string(merge_passwd_dir).map(Path::new);

    // Break hardlinks for the shadow files, since shadow-utils currently uses
    // O_RDWR unconditionally.
    for filename in PWGRP_SHADOW_FILES {
        let src = format!("etc/{filename}");
        if rootfs.try_exists(&src)? {
            ostree::break_hardlink(rootfs.as_raw_fd(), &src, true, gio::Cancellable::NONE)?;
        };
    }

    let has_usrlib_passwd = has_usrlib_passwd(&rootfs)?;
    if has_usrlib_passwd {
        prepare_pwgrp(&rootfs, merge_passwd_dir)?;
    }

    Ok(has_usrlib_passwd)
}

pub fn complete_rpm_layering(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs_dfd)? };
    complete_pwgrp(&rootfs)?;

    // /etc/shadow ends up with a timestamp in it thanks to the `lstchg`
    // field. This can be made empty safely, especially for accounts that
    // are locked.
    normalization::normalize_etc_shadow(&rootfs)?;

    Ok(())
}

/// Clean up passwd files.
///
/// This may be leftover in the tree from an older version of rpm-ostree that
/// didn't clean them up at compose time, and having them exist will mean
/// rofiles-fuse will prevent useradd from opening it for write.
pub fn passwd_cleanup(rootfs_dfd: i32) -> Result<()> {
    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs_dfd)? };
    for filename in PWGRP_LOCK_AND_BACKUP_FILES {
        let target = format!("usr/etc/{}", filename);
        rootfs.remove_file_optional(target)?;
    }

    Ok(())
}

/// Passwd splitting logic.
///
/// This function is taking the /etc/passwd generated in the install root (really
/// in /usr/etc at this point), and splitting it into two streams: a new
/// /etc/passwd that just contains the root entry, and /usr/lib/passwd which
/// contains everything else.
#[context("Migrating 'passwd' to /usr/lib")]
pub fn migrate_passwd_except_root(rootfs_dfd: i32) -> CxxResult<()> {
    static ETCSRC_PATH: &str = "usr/etc/passwd";
    static USRDEST_PATH: &str = "usr/lib/passwd";

    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs_dfd)? };
    let (roots, others): (Vec<_>, Vec<_>) = {
        let src_rd = rootfs.open(ETCSRC_PATH).map(BufReader::new)?;
        let entries = nameservice::passwd::parse_passwd_content(src_rd)?;
        entries.into_iter().partition(|e| e.uid == 0)
    };

    {
        let mut options = OpenOptions::new();
        options.create(true).append(true).mode(DEFAULT_MODE);
        let mut usrdest_stream = rootfs
            .open_with(USRDEST_PATH, &options)
            .map(BufWriter::new)
            .with_context(|| format!("Creating {USRDEST_PATH}"))?;

        for entry in &others {
            entry.to_writer(&mut usrdest_stream)?;
        }

        usrdest_stream.flush()?;
    }

    rootfs.atomic_replace_with(ETCSRC_PATH, |etcdest_stream| -> Result<()> {
        etcdest_stream
            .get_mut()
            .as_file_mut()
            .set_permissions(DEFAULT_PERMS.clone())?;
        for entry in &roots {
            entry.to_writer(etcdest_stream)?;
        }
        Ok(())
    })?;

    Ok(())
}

/// Group splitting logic.
///
/// This function is taking the /etc/group generated in the install root (really
/// in /usr/etc at this point), and splitting it into two streams: a new
/// /etc/group that just contains roots and preserved entries, and /usr/lib/group
/// which contains everything else.
#[context("Migrating 'group' to /usr/lib")]
pub fn migrate_group_except_root(rootfs_dfd: i32, preserved_groups: &Vec<String>) -> CxxResult<()> {
    static ETCSRC_PATH: &str = "usr/etc/group";
    static USRDEST_PATH: &str = "usr/lib/group";

    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs_dfd)? };
    let (mut roots_preserved, others): (Vec<_>, Vec<_>) = {
        let src_rd = rootfs.open(ETCSRC_PATH).map(BufReader::new)?;
        let entries = nameservice::group::parse_group_content(src_rd)?;
        entries.into_iter().partition(|e| e.gid == 0)
    };

    {
        let mut options = OpenOptions::new();
        options.create(true).append(true).mode(DEFAULT_MODE);
        let mut usrdest_stream = rootfs
            .open_with(USRDEST_PATH, &options)
            .map(BufWriter::new)?;

        for entry in &others {
            entry.to_writer(&mut usrdest_stream)?;
            // If it's marked in the preserve group, we need to write to
            // *both* /etc and /usr/lib in order to preserve semantics for
            // upgraded systems from before we supported the preserve concept.
            if preserved_groups.contains(&entry.name) {
                roots_preserved.push(entry.clone())
            }
        }

        usrdest_stream.flush()?;
    }

    rootfs.atomic_replace_with(ETCSRC_PATH, |etcdest_stream| -> Result<()> {
        etcdest_stream
            .get_mut()
            .as_file_mut()
            .set_permissions(DEFAULT_PERMS.clone())?;
        for entry in &roots_preserved {
            entry.to_writer(etcdest_stream)?;
        }
        Ok(())
    })?;

    Ok(())
}

/// Recursively search a directory for a subpath owned by a UID.
pub fn dir_contains_uid(dirfd: i32, id: u32) -> CxxResult<bool> {
    let dir = unsafe { ffiutil::ffi_dirfd(dirfd)? };
    let uid = Uid::from_raw(id);
    let found = dir_contains_uid_gid(&dir, &Some(uid), &None)?;
    Ok(found)
}

/// Recursively search a directory for a subpath owned by a GID.
pub fn dir_contains_gid(dirfd: i32, id: u32) -> CxxResult<bool> {
    let dir = unsafe { ffiutil::ffi_dirfd(dirfd)? };
    let gid = Gid::from_raw(id);
    let found = dir_contains_uid_gid(&dir, &None, &Some(gid))?;
    Ok(found)
}

/// Recursively search a directory for a subpath owned by a UID or GID.
fn dir_contains_uid_gid(dir: &Dir, uid: &Option<Uid>, gid: &Option<Gid>) -> Result<bool> {
    // First check the directory itself.
    let self_metadata = dir.dir_metadata()?;
    if compare_uid_gid(self_metadata, uid, gid) {
        return Ok(true);
    }

    // Then recursively check all entries and subdirectories.
    for dir_entry in dir.entries()? {
        let dir_entry = dir_entry?;
        let dtype = dir_entry.file_type()?;

        let found_match = if dtype.is_dir() {
            let subdir = dir.open_dir(dir_entry.file_name())?;
            dir_contains_uid_gid(&subdir, uid, gid)?
        } else {
            let metadata = dir_entry.metadata()?;
            compare_uid_gid(metadata, uid, gid)
        };

        if found_match {
            return Ok(true);
        }
    }

    Ok(false)
}

/// Helper for checking UID/GID stat fields.
fn compare_uid_gid(metadata: cap_std::fs::Metadata, uid: &Option<Uid>, gid: &Option<Gid>) -> bool {
    let mut found = false;
    if let Some(raw_uid) = uid.map(|u| u.as_raw()) {
        if metadata.uid() == raw_uid {
            found |= true;
        };
    }
    if let Some(raw_gid) = gid.map(|u| u.as_raw()) {
        if metadata.gid() == raw_gid {
            found |= true;
        };
    }
    found
}

pub fn passwd_compose_prep(rootfs_dfd: i32, treefile: &mut Treefile) -> CxxResult<()> {
    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs_dfd)? };
    passwd_compose_prep_impl(&rootfs, treefile, None, true)?;
    Ok(())
}

/// Passwd/group handler for composes/treefiles.
///
/// We support various passwd/group handling. This function is primarily
/// responsible for handling the "previous" and "file" paths; in both
/// cases we inject data into the tree before even laying
/// down any files, and notably before running RPM `useradd` etc.
pub fn passwd_compose_prep_repo(
    rootfs_dfd: i32,
    treefile: &mut Treefile,
    ffi_repo: &crate::ffi::OstreeRepo,
    previous_checksum: &str,
    unified_core: bool,
) -> Result<()> {
    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs_dfd)? };
    let repo = ffi_repo.glib_reborrow();
    // C side uses "" for None
    let repo_previous_rev = if previous_checksum.is_empty() {
        None
    } else {
        Some((repo.as_ref(), previous_checksum))
    };
    passwd_compose_prep_impl(&rootfs, treefile, repo_previous_rev, unified_core)
}

fn passwd_compose_prep_impl(
    rootfs: &Dir,
    treefile: &mut Treefile,
    repo_previous_rev: Option<(&ostree::Repo, &str)>,
    unified_core: bool,
) -> Result<()> {
    let generate_from_previous = treefile.parsed.base.preserve_passwd.unwrap_or(true);
    if !generate_from_previous {
        // Nothing to do
        return Ok(());
    };

    let dest = if unified_core { "usr/etc/" } else { "etc/" };

    // Create /etc in the target root; FIXME - should ensure we're using
    // the right permissions from the filesystem RPM.  Doing this right
    // is really hard because filesystem depends on setup which installs
    // the files...
    let mut db = cap_std::fs::DirBuilder::new();
    db.recursive(true);
    db.mode(0o755);
    rootfs.create_dir_with(dest, &db)?;

    // TODO(lucab): consider reworking these to avoid boolean results.
    let found_passwd_data = write_data_from_treefile(rootfs, treefile, dest, &PasswdKind::User)?;
    let found_groups_data = write_data_from_treefile(rootfs, treefile, dest, &PasswdKind::Group)?;

    // We should error if we are getting passwd data from JSON and group from
    // previous commit, or vice versa, as that'll confuse everyone when it goes
    // wrong.
    match (found_passwd_data, found_groups_data) {
        (true, false) => {
            anyhow::bail!("configured to migrate passwd data from JSON, and group data from commit")
        }
        (false, true) => {
            anyhow::bail!("configured to migrate passwd data from commit, and group data from JSON")
        }
        _ => {}
    };

    if !found_passwd_data {
        if let Some((repo, prev_rev)) = repo_previous_rev {
            concat_fs_content(rootfs, repo, prev_rev)?;
        }
    }

    Ok(())
}

// PasswdKind includes 2 types: user and group.
#[derive(Debug)]
enum PasswdKind {
    User,
    Group,
}

impl PasswdKind {
    // Get user/group passwd file
    fn passwd_file(&self) -> &'static str {
        match *self {
            PasswdKind::User => "passwd",
            PasswdKind::Group => "group",
        }
    }
    // Get user/group shadow file
    fn shadow_file(&self) -> &'static str {
        match *self {
            PasswdKind::User => "shadow",
            PasswdKind::Group => "gshadow",
        }
    }
}

/// Due to a prior bug, the build system had some deployments with a world-readable
/// shadow file.  This fixes a given deployment.
#[context("Fixing shadow permissions")]
pub(crate) fn fix_shadow_perms_in_root(root: &Dir) -> Result<bool> {
    let zero_perms = Permissions::from_mode(0);
    let mut changed = false;
    for path in ["etc/shadow", "etc/shadow-", "etc/gshadow", "etc/gshadow-"] {
        let metadata = if let Some(meta) = root
            .symlink_metadata_optional(path)
            .context("Querying metadata")?
        {
            meta
        } else {
            tracing::debug!("No path {path}");
            continue;
        };
        let mode = metadata.mode() & !libc::S_IFMT;
        // Don't touch the file if it's already correct
        if mode == 0 {
            continue;
        }
        let f = root.open(path).with_context(|| format!("Opening {path}"))?;
        f.set_permissions(zero_perms.clone())
            .with_context(|| format!("chmod: {path}"))?;
        println!("Adjusted mode for {path}");
        changed = true;
    }
    // Write our stamp file
    root.write(SHADOW_MODE_FIXED_STAMP, "")
        .context(SHADOW_MODE_FIXED_STAMP)?;
    // And clean up the old one
    root.remove_file_optional(SHADOW_MODE_FIXED_STAMP_OLD)
        .with_context(|| format!("Removing old {SHADOW_MODE_FIXED_STAMP_OLD}"))?;
    Ok(changed)
}

/// Due to a prior bug, the build system had some deployments with a world-readable
/// shadow file.  This fixes all deployments.
pub(crate) fn fix_shadow_perms_in_sysroot(sysroot: &ostree::Sysroot) -> Result<bool> {
    let deployments = sysroot.deployments();
    // TODO add a nicer api for this to ostree-rs
    let sysroot_fd =
        Dir::reopen_dir(unsafe { &std::os::fd::BorrowedFd::borrow_raw(sysroot.fd()) })?;
    let mut changed = false;
    for deployment in deployments {
        let path = sysroot.deployment_dirpath(&deployment);
        let dir = sysroot_fd.open_dir(&path)?;
        if fix_shadow_perms_in_root(&dir)
            .with_context(|| format!("Deployment index={}", deployment.index()))?
        {
            println!(
                "Adjusted shadow files in deployment index={} {}.{}",
                deployment.index(),
                deployment.csum(),
                deployment.bootserial()
            );
            changed = true;
        }
    }
    Ok(changed)
}

/// The main entrypoint for updating /etc/{,g}shadow permissions across
/// all deployments.
pub fn fix_shadow_perms_entrypoint(_args: &[&str]) -> Result<()> {
    let cancellable = gio::Cancellable::NONE;
    let sysroot = ostree::Sysroot::new_default();
    sysroot.set_mount_namespace_in_use();
    sysroot.lock()?;
    sysroot.load(cancellable)?;
    let changed = fix_shadow_perms_in_sysroot(&sysroot)?;
    if changed {
        // We already printed per deployment, so this one is just
        // a debug-level log.
        tracing::debug!("Updated shadow/gshadow permissions");
    }
    sysroot.unlock();
    Ok(())
}

// This function writes the static passwd/group data from the treefile to the
// target root filesystem.
fn write_data_from_treefile(
    rootfs: &Dir,
    treefile: &mut Treefile,
    dest_path: &str,
    target: &PasswdKind,
) -> Result<bool> {
    anyhow::ensure!(!dest_path.is_empty(), "missing destination path");

    let append_unique_entries = match target {
        PasswdKind::User => passwd_append_unique,
        PasswdKind::Group => group_append_unique,
    };

    let passwd_name = target.passwd_file();
    let target_passwd_path = format!("{}{}", dest_path, passwd_name);

    // Migrate the check data from the specified file to /etc.
    let mut src_file = match target {
        PasswdKind::User => {
            let check_passwd_file = match treefile.parsed.get_check_passwd() {
                CheckPasswd::File(cfg) => cfg,
                _ => return Ok(false),
            };
            treefile.externals.passwd_file_mut(check_passwd_file)?
        }
        PasswdKind::Group => {
            let check_groups_file = match treefile.parsed.get_check_groups() {
                CheckGroups::File(cfg) => cfg,
                _ => return Ok(false),
            };
            treefile.externals.group_file_mut(check_groups_file)?
        }
    };

    let mut seen_names = HashSet::new();
    rootfs
        .atomic_replace_with(&target_passwd_path, |dest_bufwr| -> Result<()> {
            dest_bufwr
                .get_mut()
                .as_file_mut()
                .set_permissions(DEFAULT_PERMS.clone())?;
            let mut buf_rd = BufReader::new(&mut src_file);
            append_unique_entries(&mut buf_rd, &mut seen_names, dest_bufwr).with_context(|| {
                format!("failed to process '{}' content from JSON", &passwd_name)
            })?;
            Ok(())
        })
        .with_context(|| format!("failed to write /{}", &target_passwd_path))?;

    // Regenerate etc/{,g}shadow to sync with etc/{passwd,group}
    let db = rootfs.open(target_passwd_path).map(BufReader::new)?;
    let shadow_name = target.shadow_file();
    let target_shadow_path = format!("{}{}", dest_path, shadow_name);
    // Ideally these permissions come from `setup`, which is the package
    // that owns these files:
    // https://src.fedoraproject.org/rpms/setup/blob/c6f58b338bd3/f/setup.spec#_96
    // But at this point of the compose, the rootfs is completely empty; we
    // haven't started unpacking things yet. So we need to hardcode it here.
    let shadow_perms = cap_std::fs::Permissions::from_mode(0);

    match target {
        PasswdKind::User => {
            let entries = nameservice::passwd::parse_passwd_content(db)?;
            rootfs
                .atomic_replace_with(&target_shadow_path, |target_shadow| -> Result<()> {
                    for user in entries {
                        writeln!(target_shadow, "{}:*::0:99999:7:::", user.name)?;
                    }
                    target_shadow
                        .get_mut()
                        .as_file_mut()
                        .set_permissions(shadow_perms)?;
                    Ok(())
                })
                .with_context(|| format!("Writing {target_shadow_path}"))?;
        }
        PasswdKind::Group => {
            let entries = nameservice::group::parse_group_content(db)?;
            rootfs
                .atomic_replace_with(&target_shadow_path, |target_shadow| -> Result<()> {
                    for group in entries {
                        writeln!(target_shadow, "{}:::", group.name)?;
                    }
                    target_shadow
                        .get_mut()
                        .as_file_mut()
                        .set_permissions(shadow_perms)?;
                    Ok(())
                })
                .with_context(|| format!("Writing {target_shadow_path}"))?;
        }
    }
    Ok(true)
}

/// Merge and deduplicate entries from /usr/etc and /usr/lib.
fn concat_fs_content(rootfs: &Dir, repo: &ostree::Repo, previous_checksum: &str) -> Result<()> {
    anyhow::ensure!(!previous_checksum.is_empty(), "missing previous reference");

    let (prev_root, _name) = repo.read_commit(previous_checksum, gio::Cancellable::NONE)?;

    concat_files(rootfs, &prev_root, "passwd")
        .context("failed to merge entries into /etc/passwd")?;
    concat_files(rootfs, &prev_root, "group").context("failed to merge entries into /etc/group")?;

    Ok(())
}

fn concat_files(rootfs: &Dir, prev_root: &gio::File, target: &str) -> Result<()> {
    let append_unique_fn = match target {
        "passwd" => passwd_append_unique,
        "group" => group_append_unique,
        x => anyhow::bail!("invalid merge target '{}'", x),
    };

    let orig_usretc_content = {
        let usretc_src = format!("usr/etc/{}", &target);
        prev_root.resolve_relative_path(usretc_src)
    };
    let orig_usrlib_content = {
        let usrlib_src = format!("usr/lib/{}", &target);
        prev_root.resolve_relative_path(usrlib_src)
    };

    if !(orig_usretc_content.query_exists(gio::Cancellable::NONE)
        && orig_usrlib_content.query_exists(gio::Cancellable::NONE))
    {
        // This could actually happen after we transition to systemd-sysusers;
        // we won't have a need for preallocated user data in the tree.
        return Ok(());
    }

    let etc_target = format!("etc/{}", target);
    rootfs
        .atomic_replace_with(&etc_target, |dest_bufwr| -> Result<()> {
            dest_bufwr
                .get_mut()
                .as_file_mut()
                .set_permissions(DEFAULT_PERMS.clone())?;
            let mut seen_names = HashSet::new();
            if orig_usretc_content.query_exists(gio::Cancellable::NONE) {
                let src_stream = orig_usretc_content
                    .read(gio::Cancellable::NONE)?
                    .into_read();
                let mut buf_rd = BufReader::new(src_stream);
                append_unique_fn(&mut buf_rd, &mut seen_names, dest_bufwr)
                    .with_context(|| format!("failed to process /usr/etc/{}", &target))?;
            }
            if orig_usrlib_content.query_exists(gio::Cancellable::NONE) {
                let src_stream = orig_usrlib_content
                    .read(gio::Cancellable::NONE)?
                    .into_read();
                let mut buf_rd = BufReader::new(src_stream);
                append_unique_fn(&mut buf_rd, &mut seen_names, dest_bufwr)
                    .with_context(|| format!("failed to process /usr/lib/{}", &target))?;
            };
            Ok(())
        })
        .with_context(|| format!("failed to write /etc/{}", &target))?;

    Ok(())
}

fn passwd_append_unique(
    src_bufrd: &mut impl BufRead,
    seen: &mut HashSet<String>,
    dest: &mut BufWriter<cap_tempfile::TempFile>,
) -> Result<()> {
    let entries = nameservice::passwd::parse_passwd_content(src_bufrd)?;
    for passwd in entries {
        if seen.contains(&passwd.name) {
            continue;
        }
        seen.insert(passwd.name.clone());
        passwd.to_writer(dest)?;
    }
    Ok(())
}

fn group_append_unique(
    src_bufrd: &mut impl BufRead,
    seen: &mut HashSet<String>,
    dest: &mut BufWriter<cap_tempfile::TempFile>,
) -> Result<()> {
    let entries = nameservice::group::parse_group_content(src_bufrd)?;
    for group in entries {
        if seen.contains(&group.name) {
            continue;
        }
        seen.insert(group.name.clone());
        group.to_writer(dest)?;
    }
    Ok(())
}

fn has_usrlib_passwd(rootfs: &Dir) -> std::io::Result<bool> {
    // Does this rootfs have a usr/lib/passwd? We might be doing a
    // container or something else.
    rootfs.try_exists("usr/lib/passwd")
}

#[context("Preparing pwgrp")]
fn prepare_pwgrp(rootfs: &Dir, merge_passwd_dir: Option<&Path>) -> Result<()> {
    for filename in USRLIB_PWGRP_FILES {
        let etc_file = format!("etc/{}", filename);
        let etc_backup = format!("{}.rpmostreesave", etc_file);
        let usrlib_file = format!("usr/lib/{}", filename);

        // Retain the current copies in /etc as backups.
        rootfs
            .rename(&etc_file, rootfs, &etc_backup)
            .with_context(|| format!("Renaming original {etc_file}"))?;

        // Copy /usr/lib/{passwd,group} -> /etc (breaking hardlinks).
        {
            let mut src = std::io::BufReader::new(rootfs.open(&usrlib_file)?);
            rootfs
                .atomic_replace_with(&etc_file, |w| std::io::copy(&mut src, w))
                .with_context(|| format!("Writing {etc_file}"))?;
        }

        // Copy the merge's passwd/group to usr/lib (breaking hardlinks).
        if let Some(ref merge_dir) = merge_passwd_dir {
            let current_root = Dir::open_ambient_dir(merge_dir, cap_std::ambient_authority())?;
            let mut src = current_root
                .open(filename)
                .map(BufReader::new)
                .with_context(|| format!("Opening {filename}"))?;
            rootfs
                .atomic_replace_with(&usrlib_file, |mut w| std::io::copy(&mut src, &mut w))
                .with_context(|| format!("Writing {usrlib_file}"))?;
        }
    }

    Ok(())
}

fn complete_pwgrp(rootfs: &Dir) -> Result<()> {
    for filename in USRLIB_PWGRP_FILES {
        // And now the inverse: /etc/passwd -> /usr/lib/passwd
        let etc_file = format!("etc/{}", filename);
        let usrlib_file = format!("usr/lib/{}", filename);
        rootfs.rename(&etc_file, rootfs, &usrlib_file)?;

        // /etc/passwd.rpmostreesave -> /etc/passwd */
        let etc_backup = format!("{}.rpmostreesave", etc_file);
        rootfs.rename(&etc_backup, rootfs, &etc_file)?;
    }

    // However, we leave the (potentially modified) shadow files in place.
    // In actuality, nothing should change /etc/shadow or /etc/gshadow, so
    // we'll just have to pay the (tiny) cost of re-checksumming.

    Ok(())
}

/// Validate users/groups according to treefile check-passwd/check-groups configuration.
///
/// This is a pre-commit validation hook which ensures that the upcoming
/// users/groups entries are somehow sane. See treefile `check-passwd` and
/// `check-groups` fields for a description of available validation knobs.
pub fn check_passwd_group_entries(
    ffi_repo: &crate::ffi::OstreeRepo,
    rootfs_dfd: i32,
    treefile: &mut Treefile,
    previous_rev: &str,
) -> CxxResult<()> {
    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs_dfd)? };
    let repo = ffi_repo.glib_reborrow();

    let mut repo_previous_rev = None;
    if !previous_rev.is_empty() {
        repo_previous_rev = Some((repo.as_ref(), previous_rev));
    }

    // Parse entries in the upcoming commit content.
    let mut new_entities = PasswdEntries::default();
    new_entities.add_passwd_content(rootfs.as_raw_fd(), "usr/lib/passwd")?;
    new_entities.add_group_content(rootfs.as_raw_fd(), "usr/lib/group")?;

    // Fetch entries from treefile and previous commit, according to config.
    // These are used as ground-truth by the validation steps below.
    let mut old_entities = PasswdEntries::default();
    old_entities.populate_users_from_treefile(treefile, &repo_previous_rev)?;
    old_entities.populate_groups_from_treefile(treefile, &repo_previous_rev)?;

    // See "man 5 passwd". We just make sure the name and uid/gid match,
    // and that none are missing. Don't care about GECOS/dir/shell.
    new_entities.validate_treefile_check_passwd(
        &old_entities,
        rootfs.as_raw_fd(),
        &treefile.parsed.base.ignore_removed_users,
    )?;

    // See "man 5 group". We just need to make sure the name and gid match,
    // and that none are missing. Don't care about users.
    new_entities.validate_treefile_check_groups(
        &old_entities,
        rootfs.as_raw_fd(),
        &treefile.parsed.base.ignore_removed_groups,
    )?;

    Ok(())
}

/// Database holding users and groups.
// TODO(lucab): consider folding this into `PasswdEntries`.
#[derive(Debug, Default)]
pub struct PasswdDB {
    users: HashMap<Uid, String>,
    groups: HashMap<Gid, String>,
}

/// Populate a new DB with content from `passwd` and `group` files.
pub fn passwddb_open(rootfs: i32) -> CxxResult<Box<PasswdDB>> {
    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs)? };
    let db = PasswdDB::populate_new(&rootfs)?;
    Ok(Box::new(db))
}

impl PasswdDB {
    /// Populate a new DB with content from `passwd` and `group` files.
    #[context("Populating users and groups DB")]
    pub(crate) fn populate_new(rootfs: &Dir) -> Result<Self> {
        let mut db = Self::default();
        db.add_passwd_content(rootfs.as_raw_fd(), "usr/etc/passwd")?;
        db.add_passwd_content(rootfs.as_raw_fd(), "usr/lib/passwd")?;
        db.add_group_content(rootfs.as_raw_fd(), "usr/etc/group")?;
        db.add_group_content(rootfs.as_raw_fd(), "usr/lib/group")?;
        Ok(db)
    }

    /// Lookup user name by ID.
    pub fn lookup_user(&self, uid: u32) -> CxxResult<String> {
        let key = Uid::from_raw(uid);
        let username = self
            .users
            .get(&key)
            .cloned()
            .ok_or_else(|| anyhow!("failed to find user ID '{}'", uid))?;
        Ok(username)
    }

    /// Lookup group name by ID.
    pub fn lookup_group(&self, gid: u32) -> CxxResult<String> {
        let key = Gid::from_raw(gid);
        let groupname = self
            .groups
            .get(&key)
            .cloned()
            .ok_or_else(|| anyhow!("failed to find group ID '{}'", gid))?;
        Ok(groupname)
    }

    /// Add content from a `group` file.
    #[context("Parsing groups from /{}", group_path)]
    fn add_group_content(&mut self, rootfs_dfd: i32, group_path: &str) -> Result<()> {
        let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs_dfd)? };
        let db = rootfs.open(group_path)?;
        let entries = nameservice::group::parse_group_content(BufReader::new(db))?;

        for group in entries {
            let id = Gid::from_raw(group.gid);
            self.groups.insert(id, group.name);
        }
        Ok(())
    }

    /// Add content from a `passwd` file.
    #[context("Parsing users from /{}", passwd_path)]
    fn add_passwd_content(&mut self, rootfs_dfd: i32, passwd_path: &str) -> Result<()> {
        let rootfs = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
        let db = rootfs.open(passwd_path).map(BufReader::new)?;
        let entries = nameservice::passwd::parse_passwd_content(db)?;

        for user in entries {
            let id = Uid::from_raw(user.uid);
            self.users.insert(id, user.name);
        }
        Ok(())
    }
}

/// Database holding users and groups (keyed by entry name).
#[derive(Debug, Default)]
pub struct PasswdEntries {
    users: BTreeMap<String, (Uid, Gid)>,
    groups: BTreeMap<String, Gid>,
}

/// Create a new empty DB.
pub fn new_passwd_entries() -> Box<PasswdEntries> {
    Box::new(PasswdEntries::default())
}

impl PasswdEntries {
    /// Add all groups from a given `group` file.
    pub fn add_group_content(&mut self, rootfs_dfd: i32, group_path: &str) -> CxxResult<()> {
        let rootfs = unsafe { crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
        let db = rootfs.open(group_path).map(BufReader::new)?;
        let entries = nameservice::group::parse_group_content(db)?;

        for group in entries {
            let id = Gid::from_raw(group.gid);
            self.groups.insert(group.name, id);
        }
        Ok(())
    }

    /// Add all users from a given `passwd` file.
    pub fn add_passwd_content(&mut self, rootfs_dfd: i32, passwd_path: &str) -> CxxResult<()> {
        let rootfs = unsafe { crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
        let db = rootfs.open(passwd_path).map(BufReader::new)?;
        let entries = nameservice::passwd::parse_passwd_content(db)?;

        for user in entries {
            let uid = Uid::from_raw(user.uid);
            let gid = Gid::from_raw(user.gid);
            self.users.insert(user.name, (uid, gid));
        }
        Ok(())
    }

    /// Check whether the given username exists among user entries.
    pub fn contains_user(&self, username: &str) -> bool {
        self.users.contains_key(username)
    }

    /// Check whether the given groupname exists among group entries.
    pub fn contains_group(&self, groupname: &str) -> bool {
        self.groups.contains_key(groupname)
    }

    /// Lookup user ID by name.
    pub fn lookup_user_id(&self, username: &str) -> CxxResult<u32> {
        let username = self
            .users
            .get(username)
            .map(|user| user.0.as_raw())
            .ok_or_else(|| anyhow!("failed to find user '{}'", username))?;
        Ok(username)
    }

    /// Lookup group ID by name.
    pub fn lookup_group_id(&self, groupname: &str) -> CxxResult<u32> {
        let groupname = self
            .groups
            .get(groupname)
            .map(|gid| gid.as_raw())
            .ok_or_else(|| anyhow!("failed to find group '{}'", groupname))?;
        Ok(groupname)
    }

    #[context("Rendering user entries from treefile check-passwd")]
    fn populate_users_from_treefile(
        &mut self,
        treefile: &mut Treefile,
        repo_previous_rev: &Option<(&ostree::Repo, &str)>,
    ) -> Result<()> {
        let config = treefile.parsed.get_check_passwd();

        match config {
            CheckPasswd::None => {}
            CheckPasswd::File(f) => {
                let fp = treefile.externals.passwd_file_mut(f)?;
                let buf_rd = BufReader::new(fp);
                let entries = nameservice::passwd::parse_passwd_content(buf_rd)?;
                for user in entries {
                    self.users.insert(
                        user.name,
                        (Uid::from_raw(user.uid), Gid::from_raw(user.gid)),
                    );
                }
            }
            CheckPasswd::Previous => {
                // This logic short-circuits if there is no previous commit, or if
                // it doesn't contain a passwd file. Nothing to validate in that case.
                let (repo, previous_rev) = match repo_previous_rev {
                    Some(v) => v,
                    None => return Ok(()),
                };
                let (prev_root, _name) = repo.read_commit(previous_rev, gio::Cancellable::NONE)?;
                let old_path = prev_root.resolve_relative_path("usr/lib/passwd");
                if !old_path.query_exists(gio::Cancellable::NONE) {
                    return Ok(());
                }
                let old_passwd_stream = old_path.read(gio::Cancellable::NONE)?.into_read();
                let buf_rd = BufReader::new(old_passwd_stream);
                let entries = nameservice::passwd::parse_passwd_content(buf_rd)?;
                for user in entries {
                    self.users.insert(
                        user.name,
                        (Uid::from_raw(user.uid), Gid::from_raw(user.gid)),
                    );
                }
            }
            CheckPasswd::Data(data) => {
                for user in &data.entries {
                    self.users.insert(user.0.clone(), user.1.ids());
                }
            }
        };

        Ok(())
    }

    #[context("Rendering group entries from treefile check-groups")]
    fn populate_groups_from_treefile(
        &mut self,
        treefile: &mut Treefile,
        repo_previous_rev: &Option<(&ostree::Repo, &str)>,
    ) -> Result<()> {
        let config = treefile.parsed.get_check_groups();

        match config {
            CheckGroups::None => {}
            CheckGroups::File(f) => {
                let fp = treefile.externals.group_file_mut(f)?;
                let buf_rd = BufReader::new(fp);
                let entries = nameservice::group::parse_group_content(buf_rd)?;
                for group in entries {
                    self.groups.insert(group.name, Gid::from_raw(group.gid));
                }
            }
            CheckGroups::Previous => {
                // This logic short-circuits if there is no previous commit, or if
                // it doesn't contain a group file. Nothing to validate in that case.
                let (repo, previous_rev) = match repo_previous_rev {
                    Some(v) => v,
                    None => return Ok(()),
                };
                let (prev_root, _name) = repo.read_commit(previous_rev, gio::Cancellable::NONE)?;
                let old_path = prev_root.resolve_relative_path("usr/lib/group");
                if !old_path.query_exists(gio::Cancellable::NONE) {
                    return Ok(());
                }
                let old_passwd_stream = old_path.read(gio::Cancellable::NONE)?.into_read();
                let buf_rd = BufReader::new(old_passwd_stream);
                let entries = nameservice::group::parse_group_content(buf_rd)?;
                for group in entries {
                    self.groups.insert(group.name, Gid::from_raw(group.gid));
                }
            }
            CheckGroups::Data(data) => {
                for (groupname, gid) in &data.entries {
                    let id = Gid::from_raw(*gid);
                    self.groups.insert(groupname.clone(), id);
                }
            }
        };

        Ok(())
    }

    #[context("Validating user entries according to treefile check-passwd")]
    fn validate_treefile_check_passwd(
        &self,
        old_subset: &PasswdEntries,
        rootfs: i32,
        ignored_users: &Option<BTreeSet<String>>,
    ) -> Result<()> {
        let old_user_names: BTreeSet<&str> = old_subset.users.keys().map(|s| s.as_str()).collect();
        let new_keys: BTreeSet<&str> = self.users.keys().map(|s| s.as_str()).collect();

        let ignore_all_removed = ignored_users
            .as_ref()
            .map(|s| s.contains("*"))
            .unwrap_or(false);

        // Check for users missing in the new passwd DB.
        for missing_user in old_user_names.difference(&new_keys) {
            let is_user_ignored = ignored_users
                .as_ref()
                .map(|s| s.contains(*missing_user))
                .unwrap_or(false);
            if ignore_all_removed || is_user_ignored {
                println!(
                    "Ignored user missing from new passwd file: {}",
                    missing_user
                );
                continue;
            }
            // SAFETY: `missing_user` comes from `old_subset.users.keys().difference()`,
            //  thus it's always present in `old_subset`.
            let old_user_entry = old_subset
                .users
                .get(*missing_user)
                .expect("invalid old passwd entry");
            let found_matching = dir_contains_uid(rootfs, old_user_entry.0.as_raw())?;
            if found_matching {
                anyhow::bail!("User missing from new passwd file: {}", missing_user);
            }

            println!("Unused user removed from new passwd file: {}", missing_user);
        }

        // Validate all users in the new passwd DB.
        let mut new_users = Vec::with_capacity(self.users.len());
        for (username, user_entry) in &self.users {
            let old_entry = match old_subset.users.get(username) {
                None => {
                    new_users.push(username.as_str());
                    continue;
                }
                Some(u) => u,
            };

            if user_entry.0 != old_entry.0 {
                anyhow::bail!(
                    "passwd UID changed: {} ({} to {})",
                    username,
                    old_entry.0,
                    user_entry.0,
                );
            }

            if user_entry.1 != old_entry.1 {
                anyhow::bail!(
                    "passwd GID changed: {} ({} to {})",
                    username,
                    old_entry.1,
                    user_entry.1,
                );
            }
        }
        if !new_users.is_empty() {
            println!("New passwd entries: {}", new_users.join(", "));
        }

        Ok(())
    }

    #[context("Validating group entries according to treefile check-groups")]
    fn validate_treefile_check_groups(
        &self,
        old_subset: &PasswdEntries,
        rootfs: i32,
        ignored_groups: &Option<BTreeSet<String>>,
    ) -> Result<()> {
        let old_group_names: BTreeSet<&str> =
            old_subset.groups.keys().map(|s| s.as_str()).collect();
        let new_keys: BTreeSet<&str> = self.groups.keys().map(|s| s.as_str()).collect();

        let ignore_all_removed = ignored_groups
            .as_ref()
            .map(|s| s.contains("*"))
            .unwrap_or(false);

        // Check for groups missing in the new group DB.
        for missing_group in old_group_names.difference(&new_keys) {
            let is_group_ignored = ignored_groups
                .as_ref()
                .map(|s| s.contains(*missing_group))
                .unwrap_or(false);
            if ignore_all_removed || is_group_ignored {
                println!(
                    "Ignored group missing from new group file: {}",
                    missing_group
                );
                continue;
            }
            // SAFETY: `missing_group` comes from `old_subset.groups.keys().difference()`,
            //  thus it's always present in `old_subset`.
            let old_gid = old_subset
                .groups
                .get(*missing_group)
                .expect("invalid old group entry");
            let found_matching = dir_contains_gid(rootfs, old_gid.as_raw())?;
            if found_matching {
                anyhow::bail!("Group missing from new group file: {}", missing_group);
            }

            println!(
                "Unused group removed from new group file: {}",
                missing_group
            );
        }

        // Validate all groups in the new group DB.
        let mut new_groups = Vec::with_capacity(self.groups.len());
        for (groupname, group_entry) in &self.groups {
            let old_entry = match old_subset.groups.get(groupname) {
                None => {
                    new_groups.push(groupname.as_str());
                    continue;
                }
                Some(g) => g,
            };

            if group_entry != old_entry {
                anyhow::bail!(
                    "group GID changed: {} ({} to {})",
                    groupname,
                    group_entry,
                    old_entry
                );
            }
        }
        if !new_groups.is_empty() {
            println!("New group entries: {}", new_groups.join(", "));
        }

        Ok(())
    }
}

#[test]
fn test_shadow_perms() -> Result<()> {
    let root = &cap_tempfile::tempdir(cap_std::ambient_authority())?;
    root.create_dir("etc")?;
    root.write("etc/shadow", "some shadow")?;
    root.write("etc/gshadow", "some gshadow")?;
    root.set_permissions("etc/gshadow", Permissions::from_mode(0))?;

    assert!(fix_shadow_perms_in_root(root)?);
    assert!(!root.try_exists(SHADOW_MODE_FIXED_STAMP_OLD)?);
    assert!(root.try_exists(SHADOW_MODE_FIXED_STAMP)?);
    // Verify idempotence
    assert!(!fix_shadow_perms_in_root(root)?);
    assert!(!root.try_exists(SHADOW_MODE_FIXED_STAMP_OLD)?);
    assert!(root.try_exists(SHADOW_MODE_FIXED_STAMP)?);

    Ok(())
}

#[test]
/// Verify the scenario of updating from a previously fixed root
fn test_shadow_perms_from_orig_fix() -> Result<()> {
    let root = &cap_tempfile::tempdir(cap_std::ambient_authority())?;
    root.create_dir("etc")?;
    root.write("etc/shadow", "some shadow")?;
    root.set_permissions("etc/shadow", Permissions::from_mode(0))?;
    root.write("etc/gshadow", "some gshadow")?;
    root.set_permissions("etc/gshadow", Permissions::from_mode(0))?;
    // Write the original stamp file
    root.write(SHADOW_MODE_FIXED_STAMP_OLD, "")?;

    // No changes
    assert!(!fix_shadow_perms_in_root(root)?);
    // Except we should have updated to the new stamp file
    assert!(!root.try_exists(SHADOW_MODE_FIXED_STAMP_OLD)?);
    assert!(root.try_exists(SHADOW_MODE_FIXED_STAMP)?);

    Ok(())
}
