//! APIs for interacting with `/etc/passwd` and `/etc/group`, including
//! handling the "nss-altfiles" split into `/usr/lib/{passwd,group}`.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffiutil;
use crate::nameservice;
use crate::treefile::{CheckGroups, CheckPasswd, Treefile};
use anyhow::{anyhow, Context, Result};
use fn_error_context::context;
use gio::prelude::*;
use nix::unistd::{Gid, Uid};
use openat_ext::OpenatDirExt;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fs::File;
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::os::unix::io::AsRawFd;
use std::path::PathBuf;
use std::pin::Pin;

const DEFAULT_MODE: u32 = 0o644;
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

/// Prepare passwd content before layering RPMs.
///
/// We actually want RPM to inject to /usr/lib/passwd - we
/// accomplish this by temporarily renaming /usr/lib/passwd -> /usr/etc/passwd
/// (Which appears as /etc/passwd via our compatibility symlink in the bubblewrap
/// script runner). We also copy the merge deployment's /etc/passwd to
/// /usr/lib/passwd, so that %pre scripts are aware of newly added system users
/// not in the tree's /usr/lib/passwd (through nss-altfiles in the container).
pub fn prepare_rpm_layering(rootfs_dfd: i32, merge_passwd_dir: &str) -> CxxResult<bool> {
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

pub fn complete_rpm_layering(rootfs_dfd: i32) -> CxxResult<()> {
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
#[context("Migrating 'passwd' to /usr/lib")]
pub fn migrate_passwd_except_root(rootfs_dfd: i32) -> CxxResult<()> {
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
            .append_file(USRDEST_PATH, DEFAULT_MODE)
            .map(BufWriter::new)?;

        for entry in &others {
            entry.to_writer(&mut usrdest_stream)?;
        }

        usrdest_stream.flush()?;
    }

    rootfs.write_file_with_sync(
        ETCSRC_PATH,
        DEFAULT_MODE,
        |mut etcdest_stream| -> Result<()> {
            for entry in &roots {
                entry.to_writer(&mut etcdest_stream)?;
            }
            Ok(())
        },
    )?;

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

    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let (mut roots_preserved, others): (Vec<_>, Vec<_>) = {
        let src_rd = rootfs.open_file(ETCSRC_PATH).map(BufReader::new)?;
        let entries = nameservice::group::parse_group_content(src_rd)?;
        entries.into_iter().partition(|e| e.gid == 0)
    };

    {
        let mut usrdest_stream = rootfs
            .append_file(USRDEST_PATH, DEFAULT_MODE)
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

    rootfs.write_file_with_sync(
        ETCSRC_PATH,
        DEFAULT_MODE,
        |mut etcdest_stream| -> Result<()> {
            for entry in &roots_preserved {
                entry.to_writer(&mut etcdest_stream)?;
            }
            Ok(())
        },
    )?;

    Ok(())
}

/// Recursively search a directory for a subpath owned by a UID.
pub fn dir_contains_uid(dirfd: i32, id: u32) -> CxxResult<bool> {
    let dir = ffiutil::ffi_view_openat_dir(dirfd);
    let uid = Uid::from_raw(id);
    let found = dir_contains_uid_gid(&dir, &Some(uid), &None)?;
    Ok(found)
}

/// Recursively search a directory for a subpath owned by a GID.
pub fn dir_contains_gid(dirfd: i32, id: u32) -> CxxResult<bool> {
    let dir = ffiutil::ffi_view_openat_dir(dirfd);
    let gid = Gid::from_raw(id);
    let found = dir_contains_uid_gid(&dir, &None, &Some(gid))?;
    Ok(found)
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

pub fn passwd_compose_prep(rootfs_dfd: i32, treefile: &mut Treefile) -> CxxResult<()> {
    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
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
    mut ffi_repo: Pin<&mut crate::ffi::OstreeRepo>,
    previous_checksum: &str,
    unified_core: bool,
) -> Result<()> {
    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let repo = ffi_repo.gobj_wrap();
    // C side uses "" for None
    let repo_previous_rev = if previous_checksum.is_empty() {
        None
    } else {
        Some((&repo, previous_checksum))
    };
    passwd_compose_prep_impl(&rootfs, treefile, repo_previous_rev, unified_core)
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
        let check_passwd_file = match treefile.parsed.get_check_passwd() {
            CheckPasswd::File(cfg) => cfg,
            _ => return Ok(false),
        };
        treefile.externals.passwd_file_mut(&check_passwd_file)?
    } else if target == "group" {
        let check_groups_file = match treefile.parsed.get_check_groups() {
            CheckGroups::File(cfg) => cfg,
            _ => return Ok(false),
        };
        treefile.externals.group_file_mut(&check_groups_file)?
    } else {
        unreachable!("impossible merge target '{}'", target);
    };

    let mut seen_names = HashSet::new();
    rootfs
        .write_file_with_sync(
            &target_etc_filename,
            DEFAULT_MODE,
            |dest_bufwr| -> Result<()> {
                let mut buf_rd = BufReader::new(&mut src_file);
                append_unique_entries(&mut buf_rd, &mut seen_names, dest_bufwr).with_context(
                    || format!("failed to process '{}' content from JSON", &target),
                )?;
                Ok(())
            },
        )
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

    if !(orig_usretc_content.query_exists(gio::NONE_CANCELLABLE)
        && orig_usrlib_content.query_exists(gio::NONE_CANCELLABLE))
    {
        // This could actually happen after we transition to systemd-sysusers;
        // we won't have a need for preallocated user data in the tree.
        return Ok(());
    }

    let etc_target = format!("etc/{}", target);
    rootfs
        .write_file_with_sync(&etc_target, DEFAULT_MODE, |dest_bufwr| -> Result<()> {
            let mut seen_names = HashSet::new();
            if orig_usretc_content.query_exists(gio::NONE_CANCELLABLE) {
                let src_stream = orig_usretc_content.read(gio::NONE_CANCELLABLE)?.into_read();
                let mut buf_rd = BufReader::new(src_stream);
                append_unique_fn(&mut buf_rd, &mut seen_names, dest_bufwr)
                    .with_context(|| format!("failed to process /usr/etc/{}", &target))?;
            }
            if orig_usrlib_content.query_exists(gio::NONE_CANCELLABLE) {
                let src_stream = orig_usrlib_content.read(gio::NONE_CANCELLABLE)?.into_read();
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

/// Validate users/groups according to treefile check-passwd/check-groups configuration.
///
/// This is a pre-commit validation hook which ensures that the upcoming
/// users/groups entries are somehow sane. See treefile `check-passwd` and
/// `check-groups` fields for a description of available validation knobs.
pub fn check_passwd_group_entries(
    mut ffi_repo: Pin<&mut crate::ffi::OstreeRepo>,
    rootfs_dfd: i32,
    treefile: &mut Treefile,
    previous_rev: &str,
) -> CxxResult<()> {
    let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let repo = ffi_repo.gobj_wrap();

    let mut repo_previous_rev = None;
    if !previous_rev.is_empty() {
        repo_previous_rev = Some((&repo, previous_rev));
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
        &treefile.parsed.ignore_removed_users,
    )?;

    // See "man 5 group". We just need to make sure the name and gid match,
    // and that none are missing. Don't care about users.
    new_entities.validate_treefile_check_groups(
        &old_entities,
        rootfs.as_raw_fd(),
        &treefile.parsed.ignore_removed_groups,
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
    let fd = ffiutil::ffi_view_openat_dir(rootfs);
    let db = PasswdDB::populate_new(&fd)?;
    Ok(Box::new(db))
}

impl PasswdDB {
    /// Populate a new DB with content from `passwd` and `group` files.
    #[context("Populating users and groups DB")]
    pub(crate) fn populate_new(rootfs: &openat::Dir) -> Result<Self> {
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
    #[context("Parsing users from /{}", passwd_path)]
    fn add_passwd_content(&mut self, rootfs_dfd: i32, passwd_path: &str) -> Result<()> {
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
        let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
        let db = rootfs.open_file(group_path)?;
        let entries = nameservice::group::parse_group_content(BufReader::new(db))?;

        for group in entries {
            let id = Gid::from_raw(group.gid);
            self.groups.insert(group.name, id);
        }
        Ok(())
    }

    /// Add all users from a given `passwd` file.
    pub fn add_passwd_content(&mut self, rootfs_dfd: i32, passwd_path: &str) -> CxxResult<()> {
        let rootfs = ffiutil::ffi_view_openat_dir(rootfs_dfd);
        let db = rootfs.open_file(passwd_path)?;
        let entries = nameservice::passwd::parse_passwd_content(BufReader::new(db))?;

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
                let (prev_root, _name) = repo.read_commit(previous_rev, gio::NONE_CANCELLABLE)?;
                let old_path = prev_root.resolve_relative_path("usr/lib/passwd");
                if !old_path.query_exists(gio::NONE_CANCELLABLE) {
                    return Ok(());
                }
                let old_passwd_stream = old_path.read(gio::NONE_CANCELLABLE)?.into_read();
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
                let (prev_root, _name) = repo.read_commit(previous_rev, gio::NONE_CANCELLABLE)?;
                let old_path = prev_root.resolve_relative_path("usr/lib/group");
                if !old_path.query_exists(gio::NONE_CANCELLABLE) {
                    return Ok(());
                }
                let old_passwd_stream = old_path.read(gio::NONE_CANCELLABLE)?.into_read();
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
        ignored_users: &Option<HashSet<String>>,
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
        ignored_groups: &Option<HashSet<String>>,
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
