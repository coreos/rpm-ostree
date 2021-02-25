//! APIs for interacting with `/etc/passwd` and `/etc/group`, including
//! handling the "nss-altfiles" split into `/usr/lib/{passwd,group}`.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffiutil;
use crate::nameservice;
use crate::treefile::{CheckPasswdType, Treefile};
use anyhow::{anyhow, Context, Result};
use gio::prelude::InputStreamExtManual;
use gio::FileExt;
use nix::unistd::{Gid, Uid};
use openat_ext::OpenatDirExt;
use std::collections::HashMap;
use std::collections::HashSet;
use std::fs::File;
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::os::unix::io::AsRawFd;
use std::path::PathBuf;
use std::pin::Pin;

static PWGRP_SHADOW_FILES: &[&str] = &["shadow", "gshadow", "subuid", "subgid"];
static USRLIB_PWGRP_FILES: &[&str] = &["passwd", "group"];

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

/// Populate a new DB with content from `passwd` and `group` files.
pub fn passwddb_open(rootfs: i32) -> Result<Box<PasswdDB>> {
    let fd = ffiutil::ffi_view_openat_dir(rootfs);
    PasswdDB::populate_new(&fd).map(Box::new)
}

/// Prepare passwd content before layering RPMs.
///
/// We actually want RPM to inject to /usr/lib/passwd - we
/// accomplish this by temporarily renaming /usr/lib/passwd -> /usr/etc/passwd
/// (Which appears as /etc/passwd via our compatibility symlink in the bubblewrap
/// script runner). We also copy the merge deployment's /etc/passwd to
/// /usr/lib/passwd, so that %pre scripts are aware of newly added system users
/// not in the tree's /usr/lib/passwd (through nss-altfiles in the container).
pub fn prepare_rpm_layering(rootfs_dfd: i32, merge_passwd_dir: &str) -> Result<bool> {
    passwd_cleanup(rootfs_dfd)?;
    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let dir: Option<PathBuf> = opt_string(merge_passwd_dir).map(|d| d.into());

    // Break hardlinks for the shadow files, since shadow-utils currently uses
    // O_RDWR unconditionally.
    for filename in PWGRP_SHADOW_FILES {
        let src = format!("etc/{}", filename);
        if rootfs.exists(&src)? {
            ostree::break_hardlink(rootfs.as_raw_fd(), &src, true, gio::NONE_CANCELLABLE)?;
        };
    }

    let has_usrlib_passwd = has_usrlib_passwd(&rootfs)?;
    if has_usrlib_passwd {
        prepare_pwgrp(&rootfs, dir)?;
    }

    Ok(has_usrlib_passwd)
}

pub fn complete_rpm_layering(rootfs_dfd: i32) -> Result<()> {
    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
    complete_pwgrp(&rootfs)?;

    Ok(())
}

/// Clean up passwd files.
///
/// This may be leftover in the tree from an older version of rpm-ostree that
/// didn't clean them up at compose time, and having them exist will mean
/// rofiles-fuse will prevent useradd from opening it for write.
pub fn passwd_cleanup(rootfs_dfd: i32) -> Result<()> {
    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
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
pub fn migrate_passwd_except_root(rootfs_dfd: i32) -> Result<()> {
    static ETCSRC_PATH: &str = "usr/etc/passwd";
    static USRDEST_PATH: &str = "usr/lib/passwd";

    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let (roots, others): (Vec<_>, Vec<_>) = {
        let src_rd = rootfs.open_file(ETCSRC_PATH).map(BufReader::new)?;
        let entries = nameservice::passwd::parse_passwd_content(src_rd)?;
        entries.into_iter().partition(|e| e.uid == 0)
    };

    {
        let mut usrdest_stream = rootfs
            .append_file(USRDEST_PATH, 0o664)
            .map(BufWriter::new)?;

        for entry in &others {
            entry.to_writer(&mut usrdest_stream)?;
        }

        usrdest_stream.flush()?;
    }

    rootfs.write_file_with_sync(ETCSRC_PATH, 0o664, |mut etcdest_stream| -> Result<()> {
        for entry in &roots {
            entry.to_writer(&mut etcdest_stream)?;
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
pub fn migrate_group_except_root(rootfs_dfd: i32, preserved_groups: &Vec<String>) -> Result<()> {
    static ETCSRC_PATH: &str = "usr/etc/group";
    static USRDEST_PATH: &str = "usr/lib/group";

    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let (mut roots_preserved, others): (Vec<_>, Vec<_>) = {
        let src_rd = rootfs.open_file(ETCSRC_PATH).map(BufReader::new)?;
        let entries = nameservice::group::parse_group_content(src_rd)?;
        entries.into_iter().partition(|e| e.gid == 0)
    };

    {
        let mut usrdest_stream = rootfs
            .append_file(USRDEST_PATH, 0o664)
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

    rootfs.write_file_with_sync(ETCSRC_PATH, 0o664, |mut etcdest_stream| -> Result<()> {
        for entry in &roots_preserved {
            entry.to_writer(&mut etcdest_stream)?;
        }
        Ok(())
    })?;

    Ok(())
}

/// Recursively search a directory for a subpath owned by a UID.
pub fn dir_contains_uid(dirfd: i32, id: u32) -> Result<bool> {
    let dir = ffiutil::ffi_view_openat_dir(dirfd);
    let uid = Uid::from_raw(id);
    dir_contains_uid_gid(&dir, &Some(uid), &None)
}

/// Recursively search a directory for a subpath owned by a GID.
pub fn dir_contains_gid(dirfd: i32, id: u32) -> Result<bool> {
    let dir = ffiutil::ffi_view_openat_dir(dirfd);
    let gid = Gid::from_raw(id);
    dir_contains_uid_gid(&dir, &None, &Some(gid))
}

/// Recursively search a directory for a subpath owned by a UID or GID.
fn dir_contains_uid_gid(dir: &openat::Dir, uid: &Option<Uid>, gid: &Option<Gid>) -> Result<bool> {
    use openat::SimpleType;

    // First check the directory itself.
    let self_metadata = dir.self_metadata()?;
    if compare_uid_gid(self_metadata, uid, gid) {
        return Ok(true);
    }

    // Then recursively check all entries and subdirectories.
    for dir_entry in dir.list_self()? {
        let dir_entry = dir_entry?;
        let dtype = match dir_entry.simple_type() {
            Some(t) => t,
            None => continue,
        };

        let found_match = if dtype == SimpleType::Dir {
            let subdir = dir.sub_dir(dir_entry.file_name())?;
            dir_contains_uid_gid(&subdir, uid, gid)?
        } else {
            let metadata = dir.metadata(dir_entry.file_name())?;
            compare_uid_gid(metadata, uid, gid)
        };

        if found_match {
            return Ok(true);
        }
    }

    Ok(false)
}

/// Helper for checking UID/GID stat fields.
fn compare_uid_gid(metadata: openat::Metadata, uid: &Option<Uid>, gid: &Option<Gid>) -> bool {
    let mut found = false;
    if let Some(raw_uid) = uid.map(|u| u.as_raw()) {
        if metadata.stat().st_uid == raw_uid {
            found |= true;
        };
    }
    if let Some(raw_gid) = gid.map(|u| u.as_raw()) {
        if metadata.stat().st_gid == raw_gid {
            found |= true;
        };
    }
    found
}

pub fn passwd_compose_prep(rootfs_dfd: i32, treefile: &mut Treefile) -> Result<()> {
    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
    passwd_compose_prep_impl(&rootfs, treefile, None, true)
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
    mut ffi_repo: Pin<&mut crate::ffi::OstreeRepo>,
    previous_checksum: &str,
    unified_core: bool,
) -> Result<()> {
    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let repo = ffi_repo.gobj_wrap();
    passwd_compose_prep_impl(
        &rootfs,
        treefile,
        Some((&repo, previous_checksum)),
        unified_core,
    )
}

fn passwd_compose_prep_impl(
    rootfs: &openat::Dir,
    treefile: &mut Treefile,
    repo_previous_rev: Option<(&ostree::Repo, &str)>,
    unified_core: bool,
) -> Result<()> {
    let generate_from_previous = treefile.parsed.preserve_passwd.unwrap_or(true);
    if !generate_from_previous {
        // Nothing to do
        return Ok(());
    };

    let dest = if unified_core { "usr/etc/" } else { "etc/" };

    // Create /etc in the target root; FIXME - should ensure we're using
    // the right permissions from the filesystem RPM.  Doing this right
    // is really hard because filesystem depends on setup which installs
    // the files...
    rootfs.ensure_dir_all(dest, 0o0755)?;

    // TODO(lucab): consider reworking these to avoid boolean results.
    let found_passwd_data = data_from_json(rootfs, treefile, dest, "passwd")?;
    let found_groups_data = data_from_json(rootfs, treefile, dest, "group")?;

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
            concat_fs_content(&rootfs, repo, prev_rev)?;
        }
    }

    Ok(())
}

fn data_from_json(
    rootfs: &openat::Dir,
    treefile: &mut Treefile,
    dest_path: &str,
    target: &str,
) -> Result<bool> {
    anyhow::ensure!(!dest_path.is_empty(), "missing destination path");

    let append_unique_entries = match target {
        "passwd" => passwd_append_unique,
        "group" => group_append_unique,
        x => anyhow::bail!("invalid merge target '{}'", x),
    };

    let target_etc_filename = format!("{}{}", dest_path, target);

    // Migrate the check data from the specified file to /etc.
    let mut src_file = if target == "passwd" {
        let check_passwd = match treefile.parsed.check_passwd {
            None => return Ok(false),
            Some(ref p) => p,
        };

        if check_passwd.variant != CheckPasswdType::File {
            return Ok(false);
        };

        treefile.passwd_file_mut().context("missing passwd file")?
    } else if target == "group" {
        let check_groups = match treefile.parsed.check_groups {
            None => return Ok(false),
            Some(ref p) => p,
        };

        if check_groups.variant != CheckPasswdType::File {
            return Ok(false);
        };

        treefile.group_file_mut().context("missing group file")?
    } else {
        unreachable!("impossible merge target '{}'", target);
    };

    let mut seen_names = HashSet::new();
    rootfs
        .write_file_with(&target_etc_filename, 0o664, |dest_bufwr| -> Result<()> {
            let mut buf_rd = BufReader::new(&mut src_file);
            append_unique_entries(&mut buf_rd, &mut seen_names, dest_bufwr)
                .with_context(|| format!("failed to process '{}' content from JSON", &target))?;
            dest_bufwr.flush()?;
            dest_bufwr.get_ref().sync_all()?;
            Ok(())
        })
        .with_context(|| format!("failed to write /{}", &target_etc_filename))?;

    Ok(true)
}

/// Merge and deduplicate entries from /usr/etc and /usr/lib.
fn concat_fs_content(
    rootfs: &openat::Dir,
    repo: &ostree::Repo,
    previous_checksum: &str,
) -> Result<()> {
    anyhow::ensure!(!previous_checksum.is_empty(), "missing previous reference");

    let (prev_root, _name) = repo.read_commit(previous_checksum, gio::NONE_CANCELLABLE)?;

    concat_files(&rootfs, &prev_root, "passwd")
        .context("failed to merge entries into /etc/passwd")?;
    concat_files(&rootfs, &prev_root, "group")
        .context("failed to merge entries into /etc/group")?;

    Ok(())
}

fn concat_files(rootfs: &openat::Dir, prev_root: &gio::File, target: &str) -> Result<()> {
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
    if orig_usretc_content.is_none() && orig_usrlib_content.is_none() {
        // This could actually happen after we transition to systemd-sysusers;
        // we won't have a need for preallocated user data in the tree.
        return Ok(());
    }

    let etc_target = format!("etc/{}", target);
    rootfs
        .write_file_with_sync(&etc_target, 0o664, |dest_bufwr| -> Result<()> {
            let mut seen_names = HashSet::new();
            if let Some(ref src_file) = orig_usretc_content {
                let src_stream = src_file.read(gio::NONE_CANCELLABLE)?.into_read();
                let mut buf_rd = BufReader::new(src_stream);
                append_unique_fn(&mut buf_rd, &mut seen_names, dest_bufwr)
                    .with_context(|| format!("failed to process /usr/etc/{}", &target))?;
            }
            if let Some(ref src_file) = orig_usrlib_content {
                let src_stream = src_file.read(gio::NONE_CANCELLABLE)?.into_read();
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
    dest: &mut BufWriter<File>,
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
    dest: &mut BufWriter<File>,
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

fn has_usrlib_passwd(rootfs: &openat::Dir) -> Result<bool> {
    // Does this rootfs have a usr/lib/passwd? We might be doing a
    // container or something else.
    Ok(rootfs.exists("usr/lib/passwd")?)
}

fn prepare_pwgrp(rootfs: &openat::Dir, merge_passwd_dir: Option<PathBuf>) -> Result<()> {
    for filename in USRLIB_PWGRP_FILES {
        let etc_file = format!("etc/{}", filename);
        let etc_backup = format!("{}.rpmostreesave", etc_file);
        let usrlib_file = format!("usr/lib/{}", filename);
        let usrlib_file_tmp = format!("{}.tmp", &usrlib_file);

        // Retain the current copies in /etc as backups.
        rootfs.local_rename(&etc_file, &etc_backup)?;

        // Copy /usr/lib/{passwd,group} -> /etc (breaking hardlinks).
        rootfs.copy_file(&usrlib_file, &etc_file)?;

        // Copy the merge's passwd/group to usr/lib (breaking hardlinks).
        if let Some(ref merge_dir) = merge_passwd_dir {
            {
                let current_root = openat::Dir::open("/")?;
                let merge_file = format!("{}/{}", merge_dir.display(), &filename);
                current_root.copy_file_at(&merge_file, rootfs, &usrlib_file_tmp)?;
            }
            rootfs.local_rename(&usrlib_file_tmp, &usrlib_file)?;
        }
    }

    Ok(())
}

fn complete_pwgrp(rootfs: &openat::Dir) -> Result<()> {
    for filename in USRLIB_PWGRP_FILES {
        // And now the inverse: /etc/passwd -> /usr/lib/passwd
        let etc_file = format!("etc/{}", filename);
        let usrlib_file = format!("usr/lib/{}", filename);
        rootfs.local_rename(&etc_file, &usrlib_file)?;

        // /etc/passwd.rpmostreesave -> /etc/passwd */
        let etc_backup = format!("{}.rpmostreesave", etc_file);
        rootfs.local_rename(&etc_backup, &etc_file)?;
    }

    // However, we leave the (potentially modified) shadow files in place.
    // In actuality, nothing should change /etc/shadow or /etc/gshadow, so
    // we'll just have to pay the (tiny) cost of re-checksumming.

    Ok(())
}

/// Database holding users and groups.
#[derive(Debug, Default)]
pub struct PasswdDB {
    users: HashMap<Uid, String>,
    groups: HashMap<Gid, String>,
}

impl PasswdDB {
    /// Populate a new DB with content from `passwd` and `group` files.
    pub fn populate_new(rootfs: &openat::Dir) -> anyhow::Result<Self> {
        let mut db = Self::default();
        db.add_passwd_content(rootfs.as_raw_fd(), "usr/etc/passwd")?;
        db.add_passwd_content(rootfs.as_raw_fd(), "usr/lib/passwd")?;
        db.add_group_content(rootfs.as_raw_fd(), "usr/etc/group")?;
        db.add_group_content(rootfs.as_raw_fd(), "usr/lib/group")?;
        Ok(db)
    }

    /// Lookup user name by ID.
    pub fn lookup_user(&self, uid: u32) -> anyhow::Result<String> {
        let key = Uid::from_raw(uid);
        self.users
            .get(&key)
            .cloned()
            .ok_or_else(|| anyhow!("failed to find user ID '{}'", uid))
    }

    /// Lookup group name by ID.
    pub fn lookup_group(&self, gid: u32) -> anyhow::Result<String> {
        let key = Gid::from_raw(gid);
        self.groups
            .get(&key)
            .cloned()
            .ok_or_else(|| anyhow!("failed to find group ID '{}'", gid))
    }

    /// Add content from a `group` file.
    fn add_group_content(&mut self, rootfs_dfd: i32, group_path: &str) -> anyhow::Result<()> {
        let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
        let db = rootfs.open_file(group_path)?;
        let entries = nameservice::group::parse_group_content(BufReader::new(db))?;

        for group in entries {
            let id = Gid::from_raw(group.gid);
            self.groups.insert(id, group.name);
        }
        Ok(())
    }

    /// Add content from a `passwd` file.
    fn add_passwd_content(&mut self, rootfs_dfd: i32, passwd_path: &str) -> anyhow::Result<()> {
        let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
        let db = rootfs.open_file(passwd_path)?;
        let entries = nameservice::passwd::parse_passwd_content(BufReader::new(db))?;

        for user in entries {
            let id = Uid::from_raw(user.uid);
            self.users.insert(id, user.name);
        }
        Ok(())
    }
}
