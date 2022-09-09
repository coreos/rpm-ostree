//! Implements the `cliwrap` treefile option which intercepts/proxies
//! other binaries like `/usr/bin/rpm`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, Result};
use camino::Utf8Path;
use cap_std::fs::{Dir, DirBuilder, Permissions};
use cap_std_ext::cap_std;
use cap_std_ext::prelude::CapStdExtDirExt;
use fn_error_context::context;
use rayon::prelude::*;
use std::collections::HashSet;
use std::io::prelude::*;
use std::os::unix::fs::DirBuilderExt;
use std::os::unix::prelude::PermissionsExt;
mod cliutil;
mod dracut;
mod grubby;
mod kernel_install;
mod rpm;
mod yumdnf;
use crate::cxxrsutil::CxxResult;
use crate::ffi::SystemHostType;
use crate::ffiutil::*;

/// Location for the underlying (not wrapped) binaries.
pub const CLIWRAP_DESTDIR: &str = "usr/libexec/rpm-ostree/wrapped";

/// Binaries that will be wrapped if they exist.
static WRAPPED_BINARIES: &[&str] = &["usr/bin/rpm", "usr/bin/dracut", "usr/sbin/grubby"];

/// Binaries we will wrap, or create if they don't exist.
static MUSTWRAP_BINARIES: &[&str] = &["usr/bin/yum", "usr/bin/dnf", "usr/bin/kernel-install"];

#[derive(Debug, PartialEq)]
pub(crate) enum RunDisposition {
    Ok,
    Warn,
    Notice(String),
    Unsupported,
}

/// Main entrypoint for cliwrap
pub fn entrypoint(args: &[&str]) -> Result<()> {
    // Skip the initial bits
    let args = &args[2..];
    // The outer code should always pass us at least one arg.
    let name = args
        .get(0)
        .copied()
        .ok_or_else(|| anyhow!("Missing required argument"))?;
    // Handle this case early, it's not like the other cliwrap bits.
    if name == "install-to-root" {
        return install_to_root(&args[1..]);
    }
    let name = Utf8Path::new(name)
        .file_name()
        .ok_or_else(|| anyhow!("Invalid wrapped binary: {}", name))?;

    // And now these are the args for the command
    let args = &args[1..];

    // Call original binary if environment variable is set
    if std::env::var_os("RPMOSTREE_CLIWRAP_SKIP").is_some() {
        return cliutil::exec_real_binary(name, args);
    }

    let host_type = crate::get_system_host_type()?;
    if matches!(
        host_type,
        SystemHostType::OstreeHost | SystemHostType::OstreeContainer
    ) {
        match name {
            "rpm" => Ok(self::rpm::main(host_type, args)?),
            "yum" | "dnf" => Ok(self::yumdnf::main(host_type, args)?),
            "dracut" => Ok(self::dracut::main(args)?),
            "grubby" => Ok(self::grubby::main(args)?),
            "kernel-install" => Ok(self::kernel_install::main(args)?),
            _ => Err(anyhow!("Unknown wrapped binary: {}", name)),
        }
    } else {
        // If we're not booted into ostree, just run the child directly.
        Ok(cliutil::exec_real_binary(name, args)?)
    }
}

/// Write wrappers to the target root filesystem.
fn install_to_root(args: &[&str]) -> Result<()> {
    let root = args
        .get(0)
        .map(Utf8Path::new)
        .ok_or_else(|| anyhow!("Missing required argument: ROOTDIR"))?;
    let rootdir = &Dir::open_ambient_dir(root, cap_std::ambient_authority())?;
    write_wrappers(rootdir, None)?;
    println!("Successfully enabled cliwrap for {root}");
    Ok(())
}

#[context("Writing wrapper for {:?}", binpath)]
fn write_one_wrapper(rootfs_dfd: &Dir, binpath: &Utf8Path, allow_noent: bool) -> Result<()> {
    let exists = rootfs_dfd.try_exists(binpath)?;
    if !exists && allow_noent {
        return Ok(());
    }

    let name = binpath
        .file_name()
        .ok_or_else(|| anyhow!("Invalid file name {}", binpath))?;

    let perms = Permissions::from_mode(0o755);
    if exists {
        let destpath = format!("{}/{}", CLIWRAP_DESTDIR, name);
        rootfs_dfd.rename(binpath, rootfs_dfd, destpath.as_str())?;
        rootfs_dfd.atomic_replace_with(binpath, |w| {
            w.get_mut().as_file_mut().set_permissions(perms)?;
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
        rootfs_dfd.atomic_replace_with(binpath, |w| {
            w.get_mut().as_file_mut().set_permissions(perms)?;
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
fn write_wrappers(rootfs_dfd: &Dir, allowlist: Option<&HashSet<&str>>) -> Result<()> {
    let destdir = Utf8Path::new(CLIWRAP_DESTDIR);
    let mut dirbuilder = DirBuilder::new();
    dirbuilder.mode(0o755);
    rootfs_dfd.ensure_dir_with(destdir.parent().unwrap(), &dirbuilder)?;
    rootfs_dfd.ensure_dir_with(destdir, &dirbuilder)?;

    let all_wrapped = WRAPPED_BINARIES.iter().map(Utf8Path::new);
    let all_mustwrap = MUSTWRAP_BINARIES.iter().map(Utf8Path::new);
    let all_names = all_wrapped
        .clone()
        .chain(all_mustwrap.clone())
        .map(|p| p.file_name().unwrap())
        .collect::<HashSet<_>>();

    if let Some(allowlist) = allowlist.as_ref() {
        for k in allowlist.iter() {
            if !all_names.contains(k) {
                anyhow::bail!("Unknown cliwrap binary: {k}");
            }
        }
    }

    let allowlist_contains =
        |v: &(&Utf8Path, bool)| allowlist.map_or(true, |l| l.contains(v.0.file_name().unwrap()));

    WRAPPED_BINARIES
        .par_iter()
        .map(|p| (Utf8Path::new(p), true))
        .chain(
            MUSTWRAP_BINARIES
                .par_iter()
                .map(|p| (Utf8Path::new(p), false)),
        )
        .filter(allowlist_contains)
        .try_for_each(|(binpath, allow_noent)| write_one_wrapper(rootfs_dfd, binpath, allow_noent))
}

pub(crate) fn cliwrap_write_wrappers(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs_dfd = unsafe { &ffi_dirfd(rootfs_dfd)? };
    write_wrappers(rootfs_dfd, None).map_err(Into::into)
}

pub(crate) fn cliwrap_write_some_wrappers(rootfs_dfd: i32, args: &Vec<String>) -> CxxResult<()> {
    let rootfs_dfd = unsafe { &ffi_dirfd(rootfs_dfd)? };
    let allowlist = args.iter().map(|v| v.as_str()).collect::<HashSet<_>>();
    write_wrappers(rootfs_dfd, Some(&allowlist)).map_err(Into::into)
}

pub(crate) fn cliwrap_destdir() -> String {
    // We return an owned string because it's used by C so we want c_str()
    CLIWRAP_DESTDIR.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::Context;
    use std::path::Path;

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
        write_wrappers(td, None)?;
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

    #[test]
    fn test_write_wrappers_allowlist() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;
        let mut db = DirBuilder::new();
        db.mode(0o755);
        db.recursive(true);
        for &d in &["usr/bin", "usr/libexec"] {
            td.ensure_dir_with(d, &db)?;
        }
        td.write("usr/bin/rpm", "this is rpm")?;
        td.write("usr/bin/kernel-install", "this is kernel-install")?;
        let allowlist = ["kernel-install"].into_iter().collect();
        write_wrappers(td, Some(&allowlist))?;
        assert!(file_contains(td, "usr/bin/rpm", "this is rpm")?);
        assert!(file_contains(
            td,
            "usr/bin/kernel-install",
            "binary is now located at"
        )?);
        Ok(())
    }
}
