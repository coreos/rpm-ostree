use crate::cxxrsutil;
use crate::ffiutil;
use crate::includes::*;
use anyhow::{anyhow, Result};
use c_utf8::CUtf8Buf;
use nix::unistd::{Gid, Uid};
use openat_ext::OpenatDirExt;
use std::collections::HashMap;
use std::os::unix::io::AsRawFd;
use std::path::PathBuf;

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
    let dir: Option<PathBuf> = cxxrsutil::opt_string(merge_passwd_dir).map(|d| d.into());

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
            let merge_file = format!("{}/{}", merge_dir.display(), &filename);
            let tmp_target = format!("/proc/self/fd/{}/{}", rootfs.as_raw_fd(), &usrlib_file_tmp);
            std::fs::copy(merge_file, tmp_target)?;
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

    /// Add a user ID with the associated name.
    pub fn add_user(&mut self, uid: u32, username: &str) {
        let id = Uid::from_raw(uid);
        self.users.insert(id, username.to_string());
    }

    /// Add a group ID with the associated name.
    pub fn add_group(&mut self, gid: u32, groupname: &str) {
        let id = Gid::from_raw(gid);
        self.groups.insert(id, groupname.to_string());
    }

    /// Add content from a `group` file.
    pub fn add_group_content(&mut self, rootfs: i32, group_path: &str) -> anyhow::Result<()> {
        let c_path: CUtf8Buf = group_path.to_string().into();
        let db_ptr = self as *mut Self;
        let mut gerror: *mut glib_sys::GError = std::ptr::null_mut();
        // TODO(lucab): find a replacement for `fgetgrent` and drop this.
        let res =
            unsafe { rpmostree_add_group_to_hash(rootfs, c_path.as_ptr(), db_ptr, &mut gerror) };
        ffiutil::int_gerror_to_result(res, gerror)
    }

    /// Add content from a `passwd` file.
    pub fn add_passwd_content(&mut self, rootfs: i32, passwd_path: &str) -> anyhow::Result<()> {
        let c_path: CUtf8Buf = passwd_path.to_string().into();
        let db_ptr = self as *mut Self;
        let mut gerror: *mut glib_sys::GError = std::ptr::null_mut();
        // TODO(lucab): find a replacement for `fgetpwent` and drop this.
        let res =
            unsafe { rpmostree_add_passwd_to_hash(rootfs, c_path.as_ptr(), db_ptr, &mut gerror) };
        ffiutil::int_gerror_to_result(res, gerror)
    }
}
