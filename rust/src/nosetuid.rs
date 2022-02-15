//! Implements the `nosetuid` treefile option which removes SetUID and SetGID
//! bits from all executables.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;
use fn_error_context::context;
use std::fs::File;
use std::os::unix::fs::PermissionsExt;

use crate::cxxrsutil::CxxResult;
use crate::ffiutil::*;

const Executable: u32 = 0o0111;
const SetUID: u32 = 0o4000;
const SetGID: u32 = 0o2000;

/// Remove SetUID and SetGID bits from all executables.
#[context("Removing SetUID & SetGID bits from all executables")]
fn remove_setuid(rootfs_dfd: &openat::Dir) -> Result<()> {
    for entry in rootfs_dfd.list_self() {
        match entry.file_type() {
            Symlink => continue,
            Other => continue,
            Dir => remove_setuid(entry),
            File => {
                let mode = entry.path().metadata()?.permissions().mode();
                if (mode & Executable) == 0 {
                    continue;
                }
                let new_mode = (mode & !SetUID) & !SetGID;
                if mode != new_mode {
                    println!("Stripping '{}': {:#o} -> {:#o}", entry.path().display(), mode, new_mode);
                    let file = File::open(entry.path())?;
                    let mut perms = file.metadata()?.permissions();
                    perms.set_mode(new_mode);
                    file.set_permissions(perms)?;
                }
            }
        }
    }
    Ok(())
}

pub(crate) fn nosetuid_remove_setuid(rootfs_dfd: i32) -> CxxResult<()> {
    Ok(remove_setuid(&ffi_view_openat_dir(rootfs_dfd))?)
}
