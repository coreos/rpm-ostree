use crate::ffiutil;
use crate::includes::*;
use anyhow::{anyhow, bail};
use c_utf8::CUtf8Buf;
use nix::unistd::{Gid, Uid};
use std::collections::HashMap;
use std::os::unix::io::AsRawFd;

/// Populate a new DB with content from `passwd` and `group` files.
pub(crate) fn passwddb_open(rootfs: i32) -> anyhow::Result<Box<PasswdDB>> {
    let fd = ffiutil::ffi_view_openat_dir(rootfs);
    PasswdDB::populate_new(&fd).map(|db| Box::new(db))
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
