/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

use anyhow::{bail, Result};
use std::io::prelude::*;
use std::path;

use openat_ext::OpenatDirExt;
use rayon::prelude::*;
mod cliutil;
mod dracut;
mod grubby;
mod rpm;
use crate::ffiutil::*;

/// Location for the underlying (not wrapped) binaries.
pub const CLIWRAP_DESTDIR: &str = "usr/libexec/rpm-ostree/wrapped";

/// Our list of binaries that will be wrapped.  Must be a relative path.
static WRAPPED_BINARIES: &[&str] = &["usr/bin/rpm", "usr/bin/dracut", "usr/sbin/grubby"];

#[derive(Debug, PartialEq)]
pub(crate) enum RunDisposition {
    Ok,
    Warn,
    Notice(String),
}

/// Main entrypoint for cliwrap
pub(crate) fn cliwrap_entrypoint(args: Vec<String>) -> Result<()> {
    // We'll panic here if the vector is empty, but that is intentional;
    // the outer code should always pass us at least one arg.
    let name = args[0].as_str();
    let name = match std::path::Path::new(name).file_name() {
        Some(name) => name,
        None => bail!("Invalid wrapped binary: {}", name),
    };
    // We know we had a string from above
    let name = name.to_str().unwrap();

    let args: Vec<&str> = args.iter().skip(1).map(|v| v.as_str()).collect();

    // If we're not booted into ostree, just run the child directly.
    if !cliutil::is_ostree_booted() {
        cliutil::exec_real_binary(name, &args)
    } else {
        match name {
            "rpm" => self::rpm::main(&args),
            "dracut" => self::dracut::main(&args),
            "grubby" => self::grubby::main(&args),
            _ => bail!("Unknown wrapped binary: {}", name),
        }
    }
}

/// Move the real binaries to a subdir, and replace them with
/// a shell script that calls our wrapping code.
fn write_wrappers(rootfs_dfd: &openat::Dir) -> Result<()> {
    let destdir = std::path::Path::new(CLIWRAP_DESTDIR);
    rootfs_dfd.ensure_dir(destdir.parent().unwrap(), 0o755)?;
    rootfs_dfd.ensure_dir(destdir, 0o755)?;
    WRAPPED_BINARIES.par_iter().try_for_each(|&bin| {
        let binpath = path::Path::new(bin);

        if !rootfs_dfd.exists(binpath)? {
            return Ok(());
        }

        let name = binpath.file_name().unwrap().to_str().unwrap();
        let destpath = format!("{}/{}", CLIWRAP_DESTDIR, name);
        rootfs_dfd.local_rename(bin, destpath.as_str())?;

        rootfs_dfd.write_file_with(binpath, 0o755, |w| {
            write!(
                w,
                "#!/bin/sh
    # Wrapper created by rpm-ostree to override
    # behavior of the underlying binary.  For more
    # information see `man rpm-ostree`.  The real
    # binary is now located at: {}
    exec /usr/bin/rpm-ostree cliwrap $0 \"$@\"
    ",
                binpath.to_str().unwrap()
            )
        })?;
        Ok(())
    })
}

pub(crate) fn cliwrap_write_wrappers(rootfs_dfd: i32) -> Result<()> {
    write_wrappers(&ffi_view_openat_dir(rootfs_dfd))
}

pub(crate) fn cliwrap_destdir() -> String {
    // We return an owned string because it's used by C so we want c_str()
    CLIWRAP_DESTDIR.to_string()
}
