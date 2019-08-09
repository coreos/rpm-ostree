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

use libc;
use failure::{Fallible, bail};
use openat;

use std::io;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::process::CommandExt;
use std::os::unix::io::AsRawFd;
use openat_ext::OpenatDirExt;

/// Location for arbitrary postprocessing hooks.
const ROOT_HOOKS_DIR : &str = "/etc/rpm-ostree/roothooks.d";

fn execute_hook(rootfs_dfd: &openat::Dir, name: &str, path: &str) -> Fallible<()> {
    println!("Executing hook: {}", name);
    let mut cmd = std::process::Command::new(path);
    let rootfs_fd = rootfs_dfd.as_raw_fd();
    // The unsafe{} dance doesn't gain us much, and this way
    // we're compat with older rust.
    #[allow(deprecated)]
    cmd.before_exec(move || -> io::Result<()> {
        nix::unistd::fchdir(rootfs_fd).map_err(|e| io::Error::new(io::ErrorKind::Other, e.to_string()))
    });
    let res = cmd.status()?;
    if !res.success() {
        bail!("roothook {} failed: {}", name, res);
    }
    println!("Completed hook: {}", name);
    Ok(())
}

fn enumerate_hooks(hookdir: &openat::Dir) -> Fallible<Vec<openat::Entry>> {
    let mut hooks = Vec::new();
    for ent in hookdir.list_dir(".")? {
        let ent = ent?;
        let name = match ent.file_name().to_str() {
            Some(name) => name,
            None => continue,
        };
        let ftype = hookdir.get_file_type(&ent)?;
        if ftype != openat::SimpleType::File {
            continue;
        }
        let metadata = hookdir.metadata(name)?;
        if (metadata.permissions().mode() & 0o111) == 0 {
            continue;
        }
        hooks.push(ent);
    };
    hooks.sort_by(|a, b| {
        a.file_name().cmp(b.file_name())
    });
    Ok(hooks)
}

/// Called from the core after ostree layers have been laid down.  It supports
/// executing arbitrary code from the host (which could in turn e.g. run containers)
/// to target the new root.  At this time we don't recommend executing code from
/// the target root; anyone needing to do so will need to use `bwrap` or `runc`
/// or equivalent.
/// 
/// Any error fails the change.
fn roothooks_run(rootfs_dfd: &openat::Dir) -> Fallible<()> {
    let hookdir = rootfs_dfd.sub_dir_optional(ROOT_HOOKS_DIR)?;
    let hookdir = match hookdir {
        Some(x) => x,
        None => return Ok(())
    };

    let hooks = enumerate_hooks(&hookdir)?;
    for ent in hooks {
        // Already verified file_name is utf8 above
        let name = ent.file_name().to_str().unwrap();
        let path = format!("/{}/{}", ROOT_HOOKS_DIR, name);
        execute_hook(rootfs_dfd, name, &path)?;
    };

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use rayon::prelude::*;
    use tempfile;

    #[test]
    fn enumerate() -> Fallible<()> {
        let tempdir = tempfile::tempdir()?;
        let rel_hooksdir = ROOT_HOOKS_DIR.trim_start_matches('/');
        std::fs::create_dir_all(tempdir.path().join(rel_hooksdir))?;
        let rootfs = openat::Dir::open(tempdir.path())?;
        let hooksdir = rootfs.sub_dir(rel_hooksdir)?;
        assert_eq!(0, enumerate_hooks(&hooksdir)?.len());

        let valid_hooks = [0, 4, 8, 10, 12, 19, 20, 21, 42, 87];
        let hookname = |i| { format!("{:03}hook", i) };
        valid_hooks.par_iter().try_for_each(|i| -> Fallible<()> {
            hooksdir.write_file(hookname(i), 0o755)?;
            hooksdir.write_file(format!("{:03}nonhook", i), 0o644)?;
            hooksdir.create_dir(format!("{:03}subdir", i), 0o755)?;
            Ok(())
        })?;

        let hooks = enumerate_hooks(&hooksdir)?;
        let valid_hooknames : Vec<_> = valid_hooks.iter().map(hookname).collect();
        let hooknames : Vec<_> = hooks.iter().map(|e| e.file_name().to_str().unwrap().to_string()).collect();
        assert_eq!(valid_hooknames, hooknames);

        Ok(())
    }
}

mod ffi {
    use super::*;
    use glib_sys;
    use libc;

    use crate::ffiutil::*;

    #[no_mangle]
    pub extern "C" fn ror_roothooks_run(
        rootfs_dfd: libc::c_int,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let rootfs_dfd = ffi_view_openat_dir(rootfs_dfd);
        int_glib_error(roothooks_run(&rootfs_dfd), gerror)
    }
}
pub use self::ffi::*;
