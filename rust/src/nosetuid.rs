//! Implements the `nosetuid` treefile option which removes SetUID and SetGID
//! bits from all executables.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;
use fn_error_context::context;
use openat::SimpleType;
use openat_ext::OpenatDirExt;
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
    let dir = rootfs_dfd;
    for e in rootfs_dfd.list_self()? {
        let entry = e?;
        match rootfs_dfd.get_file_type(&entry)? {
            SimpleType::Symlink => continue,
            SimpleType::Other => continue,
            SimpleType::Dir => remove_setuid(&dir.sub_dir(entry.file_name())?)?,
            SimpleType::File => {
                let mode = dir
                    .open_file(entry.file_name())?
                    .metadata()?
                    .permissions()
                    .mode();
                if (mode & Executable) == 0 {
                    continue;
                }
                let new_mode = (mode & !SetUID) & !SetGID;
                if mode != new_mode {
                    println!(
                        "Stripping '{:?}': {:#o} -> {:#o}",
                        entry.file_name(),
                        mode,
                        new_mode
                    );
                    dir.set_mode(entry.file_name(), new_mode)?;
                }
            }
        }
    }
    Ok(())
}

pub(crate) fn nosetuid_remove_setuid(rootfs_dfd: i32) -> CxxResult<()> {
    Ok(remove_setuid(&ffi_view_openat_dir(rootfs_dfd))?)
}
