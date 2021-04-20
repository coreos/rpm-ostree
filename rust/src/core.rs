//! Code mirroring rpmostree-core.cxx which is the shared "core"
//! binding of rpm and ostree, used by both client-side layering/overrides
//! and server side composes.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::CxxResult;
use crate::ffiutil;
use ffiutil::*;
use openat_ext::OpenatDirExt;

/// Guard for running logic in a context with temporary /etc.
///
/// We have a messy dance in dealing with /usr/etc and /etc; the
/// current model is basically to have it be /etc whenever we're running
/// any code.
#[derive(Debug)]
pub struct TempEtcGuard {
    rootfs: openat::Dir,
    renamed_etc: bool,
}

/// Detect if we have /usr/etc and no /etc, and rename if so.
pub(crate) fn prepare_tempetc_guard(rootfs: i32) -> CxxResult<Box<TempEtcGuard>> {
    let rootfs = ffi_view_openat_dir(rootfs);
    let has_etc = rootfs.exists("etc")?;
    let mut renamed_etc = false;
    if !has_etc && rootfs.exists("usr/etc")? {
        // In general now, we place contents in /etc when running scripts
        rootfs.local_rename("usr/etc", "etc")?;
        // But leave a compat symlink, as we used to bind mount, so scripts
        // could still use that too.
        rootfs.symlink("usr/etc", "../etc")?;
        renamed_etc = true;
    }
    Ok(Box::new(TempEtcGuard {
        rootfs,
        renamed_etc,
    }))
}

impl TempEtcGuard {
    /// Remove the temporary /etc, and destroy the guard.
    pub(crate) fn undo(&self) -> CxxResult<()> {
        if self.renamed_etc {
            /* Remove the symlink and swap back */
            self.rootfs.remove_file("usr/etc")?;
            self.rootfs.local_rename("etc", "usr/etc")?;
        }
        Ok(())
    }
}

pub(crate) fn get_systemctl_wrapper() -> &'static [u8] {
    include_bytes!("../../src/libpriv/systemctl-wrapper.sh")
}

/// Run the standard `depmod` utility.
pub(crate) fn run_depmod(rootfs_dfd: i32, kver: &str, unified_core: bool) -> CxxResult<()> {
    let args: Vec<_> = vec!["depmod", "-a", kver]
        .into_iter()
        .map(|s| s.to_string())
        .collect();
    let _ = crate::bwrap::bubblewrap_run_sync(rootfs_dfd, &args, false, unified_core)?;
    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;
    use anyhow::Result;
    use std::os::unix::prelude::*;

    #[test]
    fn basic() -> Result<()> {
        let td = tempfile::tempdir()?;
        let d = openat::Dir::open(td.path())?;
        let g = super::prepare_tempetc_guard(d.as_raw_fd())?;
        g.undo()?;
        d.ensure_dir_all("usr/etc/foo", 0o755)?;
        assert!(!d.exists("etc/foo")?);
        let g = super::prepare_tempetc_guard(d.as_raw_fd())?;
        assert!(d.exists("etc/foo")?);
        g.undo()?;
        assert!(!d.exists("etc")?);
        assert!(d.exists("usr/etc/foo")?);
        Ok(())
    }
}
