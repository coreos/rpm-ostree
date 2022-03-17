//! Implements the `cliwrap` treefile option which intercepts/proxies
//! other binaries like `/usr/bin/rpm`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, Result};
use cap_std::fs::{Dir, DirBuilder, Permissions};
use cap_std_ext::cap_std;
use cap_std_ext::prelude::CapStdExtDirExt;
use fn_error_context::context;
use rayon::prelude::*;
use std::io::prelude::*;
use std::os::unix::fs::DirBuilderExt;
use std::os::unix::prelude::PermissionsExt;
use std::path::Path;
mod cliutil;
mod dracut;
mod grubby;
mod rpm;
mod yumdnf;
use crate::cxxrsutil::CxxResult;
use crate::ffiutil::*;

/// Location for the underlying (not wrapped) binaries.
pub const CLIWRAP_DESTDIR: &str = "usr/libexec/rpm-ostree/wrapped";

/// Binaries that will be wrapped if they exist.
static WRAPPED_BINARIES: &[&str] = &["usr/bin/rpm", "usr/bin/dracut", "usr/sbin/grubby"];

/// Binaries we will wrap, or create if they don't exist.
static MUSTWRAP_BINARIES: &[&str] = &["usr/bin/yum", "usr/bin/dnf"];

#[derive(Debug, PartialEq)]
pub(crate) enum RunDisposition {
    Ok,
    Warn,
    Notice(String),
}

/// Main entrypoint for cliwrap
pub fn entrypoint(args: &[&str]) -> Result<()> {
    // Skip the initial bits
    let args = &args[2..];
    // We'll panic here if the vector is empty, but that is intentional;
    // the outer code should always pass us at least one arg.
    let name = args[0];
    let name = match std::path::Path::new(name).file_name() {
        Some(name) => name,
        None => return Err(anyhow!("Invalid wrapped binary: {}", name)),
    };
    // We know we had a string from above
    let name = name.to_str().unwrap();

    // And now these are the args for the command
    let args = &args[1..];

    // If we're not booted into ostree, just run the child directly.
    if !cliutil::is_ostree_booted() {
        Ok(cliutil::exec_real_binary(name, args)?)
    } else {
        match name {
            "rpm" => Ok(self::rpm::main(args)?),
            "yum" | "dnf" => Ok(self::yumdnf::main(args)?),
            "dracut" => Ok(self::dracut::main(args)?),
            "grubby" => Ok(self::grubby::main(args)?),
            _ => Err(anyhow!("Unknown wrapped binary: {}", name)),
        }
    }
}

#[context("Writing wrapper for {:?}", binpath)]
fn write_one_wrapper(rootfs_dfd: &Dir, binpath: &Path, allow_noent: bool) -> Result<()> {
    let exists = rootfs_dfd.try_exists(binpath)?;
    if !exists && allow_noent {
        return Ok(());
    }

    let name = binpath.file_name().unwrap().to_str().unwrap();

    let perms = Permissions::from_mode(0o755);
    if exists {
        let destpath = format!("{}/{}", CLIWRAP_DESTDIR, name);
        rootfs_dfd.rename(binpath, rootfs_dfd, destpath.as_str())?;
        rootfs_dfd.replace_file_with_perms(binpath, perms, |w| {
            indoc::writedoc! {w, r#"
#!/bin/sh
# Wrapper created by rpm-ostree to override
# behavior of the underlying binary.  For more
# information see `man rpm-ostree`.  The real
# binary is now located at: {}
exec /usr/bin/rpm-ostree cliwrap $0 "$@"
"#,  destpath }
        })?;
    } else {
        rootfs_dfd.replace_file_with_perms(binpath, perms, |w| {
            indoc::writedoc! {w, r#"
#!/bin/sh
# Wrapper created by rpm-ostree to implement this CLI interface.
# For more information see `man rpm-ostree`.
exec /usr/bin/rpm-ostree cliwrap $0 "$@"
"#}
        })?;
    }
    Ok(())
}

/// Move the real binaries to a subdir, and replace them with
/// a shell script that calls our wrapping code.
fn write_wrappers(rootfs_dfd: &Dir) -> Result<()> {
    let destdir = std::path::Path::new(CLIWRAP_DESTDIR);
    let mut dirbuilder = DirBuilder::new();
    dirbuilder.mode(0o755);
    rootfs_dfd.ensure_dir_with(destdir.parent().unwrap(), &dirbuilder)?;
    rootfs_dfd.ensure_dir_with(destdir, &dirbuilder)?;

    WRAPPED_BINARIES
        .par_iter()
        .map(|p| (Path::new(p), true))
        .chain(MUSTWRAP_BINARIES.par_iter().map(|p| (Path::new(p), false)))
        .try_for_each(|(binpath, allow_noent)| write_one_wrapper(rootfs_dfd, binpath, allow_noent))
}

pub(crate) fn cliwrap_write_wrappers(rootfs_dfd: i32) -> CxxResult<()> {
    Ok(write_wrappers(unsafe { &ffi_dirfd(rootfs_dfd)? })?)
}

pub(crate) fn cliwrap_destdir() -> String {
    // We return an owned string because it's used by C so we want c_str()
    CLIWRAP_DESTDIR.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::Context;

    fn file_contains(
        d: &Dir,
        p: impl AsRef<Path>,
        expected_contents: impl AsRef<str>,
    ) -> Result<bool> {
        let p = p.as_ref();
        let expected_contents = expected_contents.as_ref();
        let found_contents = d
            .read_to_string(p)
            .with_context(|| format!("Reading {:?}", p))?;
        Ok(found_contents.contains(expected_contents))
    }

    #[test]
    fn test_write_wrappers() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;
        let mut db = DirBuilder::new();
        db.mode(0o755);
        db.recursive(true);
        for &d in &["usr/bin", "usr/libexec"] {
            td.ensure_dir_with(d, &db)?;
        }
        td.write("usr/bin/rpm", "this is rpm")?;
        write_wrappers(td)?;
        assert!(file_contains(
            td,
            "usr/bin/rpm",
            "binary is now located at: usr/libexec/rpm-ostree/wrapped/rpm"
        )?);
        assert!(!td.try_exists("usr/sbin/grubby")?);
        assert!(file_contains(
            td,
            "usr/bin/yum",
            "to implement this CLI interface"
        )?);
        Ok(())
    }
}
