//! CLI sub-command `compose`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

pub(crate) mod commit;

use crate::cxxrsutil::CxxResult;
use anyhow::{Context, Result};
use fn_error_context::context;
use openat_ext::OpenatDirExt;
use std::ffi::{CStr, CString};
use std::io::{self, Read};

/// Prepare /dev in the target root with the API devices.
// TODO: delete this when we implement https://github.com/projectatomic/rpm-ostree/issues/729
pub fn composeutil_legacy_prep_dev(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs = crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);
    legacy_prepare_dev(&rootfs)?;
    Ok(())
}

#[context("Preparing /dev hierarchy (legacy)")]
fn legacy_prepare_dev(rootfs: &openat::Dir) -> Result<()> {
    let src_dir = openat::Dir::open("/dev")?;
    static CHARDEVS: &[&str] = &["full", "null", "random", "tty", "urandom", "zero"];

    rootfs.ensure_dir("dev", 0o755)?;
    rootfs.set_mode("dev", 0o755)?;
    let dest_dir = rootfs.sub_dir("dev")?;

    for nodename in CHARDEVS {
        let src_metadata = match src_dir.metadata_optional(*nodename)? {
            Some(m) => m,
            None => continue,
        };
        // SAFETY: devnodes entries are plain non-NUL ASCII bytes.
        let path_cstr = CString::new(*nodename).expect("unexpected NUL byte");
        make_node_at(
            &dest_dir,
            &path_cstr,
            src_metadata.stat().st_mode,
            src_metadata.stat().st_rdev,
        )
        .with_context(|| format!("Creating /dev/{}", nodename))?;
        dest_dir.set_mode(*nodename, src_metadata.stat().st_mode)?;
    }
    smoketest_dev_null(&dest_dir)?;

    Ok(())
}

#[context("Testing /dev/null in target root (is 'nodev' set?)")]
fn smoketest_dev_null(devdir: &openat::Dir) -> Result<()> {
    let mut devnull = devdir.open_file("null")?;
    let mut buf = [0u8];
    let n = devnull.read(&mut buf)?;
    assert_eq!(n, 0);
    Ok(())
}

// TODO(lucab): add a safe `mknodat` helper to nix.
fn make_node_at(
    destdir: &openat::Dir,
    pathname: &CStr,
    mode: libc::mode_t,
    dev: libc::dev_t,
) -> io::Result<()> {
    use std::os::unix::io::AsRawFd;
    let r = unsafe { libc::mknodat(destdir.as_raw_fd(), pathname.as_ptr(), mode, dev) };
    if r != 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}
