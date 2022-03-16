//! CLI sub-command `compose`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

pub(crate) mod commit;

use crate::cxxrsutil::CxxResult;
use anyhow::{Context, Result};
use cap_std::fs::{Dir, DirBuilder, Permissions};
use cap_std_ext::cap_std;
use cap_std_ext::dirext::CapStdExtDirExt;
use cap_std_ext::rustix;
use cap_std_ext::rustix::fd::AsFd;
use fn_error_context::context;
use rustix::fs::{FileType, MetadataExt};
use std::io::Read;
use std::os::unix::fs::DirBuilderExt;
use std::os::unix::prelude::PermissionsExt;

use crate::core::OSTREE_BOOTED;

/// Prepare /dev and /run in the target root with the API devices.
// TODO: delete this when we implement https://github.com/projectatomic/rpm-ostree/issues/729
pub fn composeutil_legacy_prep_dev_and_run(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs = unsafe { crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
    legacy_prepare_dev(&rootfs)?;
    rootfs.create_dir_with("run", DirBuilder::new().mode(0o755))?;
    rootfs.replace_contents_with_perms(&OSTREE_BOOTED[1..], b"", Permissions::from_mode(0o755))?;
    Ok(())
}

#[context("Preparing /dev hierarchy (legacy)")]
fn legacy_prepare_dev(rootfs: &Dir) -> Result<()> {
    let src_dir =
        Dir::open_ambient_dir("/dev", cap_std::ambient_authority()).context("Opening host /dev")?;
    static CHARDEVS: &[&str] = &["full", "null", "random", "tty", "urandom", "zero"];

    rootfs
        .ensure_dir_with("dev", DirBuilder::new().mode(0o755))
        .context("Creating dev")?;
    rootfs
        .set_permissions("dev", Permissions::from_mode(0o755))
        .context("Setting permissions on target dev")?;
    let dest_dir = &rootfs.open_dir("dev").context("Opening target dev")?;

    for &nodename in CHARDEVS {
        let src_metadata = match src_dir
            .symlink_metadata_optional(nodename)
            .context(nodename)?
        {
            Some(m) => m,
            None => continue,
        };
        let mode = rustix::fs::Mode::from_bits_truncate(src_metadata.mode());
        rustix::fs::mknodat(
            dest_dir,
            nodename,
            FileType::CharacterDevice,
            mode,
            src_metadata.rdev(),
        )
        .with_context(|| format!("Creating /dev/{}", nodename))?;
        // We bypass cap-std's abstraction here because it will try to open the target
        // file so it can use `fchmod()` which may fail for special things like `/dev/tty`.
        // We have no concerns about following symlinks because we know we just created
        // the device and there are no concurrent writers.
        rustix::fs::chmodat(&dest_dir.as_fd(), nodename, mode)
            .with_context(|| format!("Setting permissions of target {}", nodename))?;
    }
    smoketest_dev_null(dest_dir)?;

    Ok(())
}

#[context("Testing /dev/null in target root (is 'nodev' set?)")]
fn smoketest_dev_null(devdir: &Dir) -> Result<()> {
    let mut devnull = devdir.open("null")?;
    let mut buf = [0u8];
    let n = devnull.read(&mut buf)?;
    assert_eq!(n, 0);
    Ok(())
}
