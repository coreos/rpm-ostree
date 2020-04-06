/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//! # Code for CoreOS rootfs
//!
//! This backs the `rpm-ostree coreos-rootfs` CLI, which is intended as
//! a hidden implmentation detail of CoreOS style systems.  The code
//! is here in rpm-ostree as a convenience.

use anyhow::{Context, Result};
use libc;
use nix;
use openat;
use std::os::unix::prelude::*;
use structopt::StructOpt;

/// A reference to a *directory* file descriptor.  Unfortunately,
/// the openat crate always uses O_PATH which doesn't support ioctl().
pub struct RawDirFd(RawFd);

impl AsRawFd for RawDirFd {
    #[inline]
    fn as_raw_fd(&self) -> RawFd {
        self.0
    }
}

impl FromRawFd for RawDirFd {
    #[inline]
    unsafe fn from_raw_fd(fd: RawFd) -> RawDirFd {
        RawDirFd(fd)
    }
}

impl Drop for RawDirFd {
    fn drop(&mut self) {
        let fd = self.0;
        unsafe {
            libc::close(fd);
        }
    }
}

/* From /usr/include/ext2fs/ext2_fs.h */
const EXT2_IMMUTABLE_FL: libc::c_long = 0x00000010; /* Immutable file */

nix::ioctl_read!(ext2_get_flags, b'f', 1, libc::c_long);
nix::ioctl_write_ptr!(ext2_set_flags, b'f', 2, libc::c_long);

#[derive(Debug, StructOpt)]
struct SealOpts {
    /// Path to rootfs
    sysroot: String,
}

#[derive(Debug, StructOpt)]
#[structopt(name = "coreos-rootfs")]
#[structopt(rename_all = "kebab-case")]
enum Opt {
    /// Final step after changing a sysroot
    Seal(SealOpts),
}

// taken from openat code
fn to_cstr<P: openat::AsPath>(path: P) -> std::io::Result<P::Buffer> {
    path.to_path().ok_or_else(|| {
        std::io::Error::new(std::io::ErrorKind::InvalidInput, "nul byte in file name")
    })
}

/// Set the immutable bit
fn seal(opts: &SealOpts) -> Result<()> {
    let fd = unsafe {
        let fd = libc::open(
            to_cstr(opts.sysroot.as_str())?.as_ref().as_ptr(),
            libc::O_CLOEXEC | libc::O_DIRECTORY,
        );
        if fd < 0 {
            Err(std::io::Error::last_os_error())?
        } else {
            RawDirFd::from_raw_fd(fd)
        }
    };

    let mut flags: libc::c_long = 0;
    unsafe { ext2_get_flags(fd.as_raw_fd(), &mut flags as *mut libc::c_long)? };
    if flags & EXT2_IMMUTABLE_FL == 0 {
        flags |= EXT2_IMMUTABLE_FL;
        unsafe { ext2_set_flags(fd.as_raw_fd(), &flags as *const libc::c_long)? };
    }
    Ok(())
}

/// Main entrypoint
fn coreos_rootfs_main(args: &Vec<String>) -> Result<()> {
    let opt = Opt::from_iter(args.iter());
    match opt {
        Opt::Seal(ref opts) => seal(opts).context("Sealing rootfs failed")?,
    };
    Ok(())
}

mod ffi {
    use super::*;
    use glib_sys;
    use libc;

    use crate::ffiutil::*;

    #[no_mangle]
    pub extern "C" fn ror_coreos_rootfs_entrypoint(
        argv: *mut *mut libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let v: Vec<String> = unsafe { glib::translate::FromGlibPtrContainer::from_glib_none(argv) };
        int_glib_error(coreos_rootfs_main(&v), gerror)
    }
}
pub use self::ffi::*;
