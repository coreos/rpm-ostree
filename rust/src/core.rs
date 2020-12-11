use crate::ffiutil;
use anyhow::Result;
use ffiutil::ffi_view_openat_dir;
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

pub fn prepare_tempetc_guard(rootfs: i32) -> Result<Box<TempEtcGuard>> {
    let rootfs = ffi_view_openat_dir(rootfs);
    let has_usretc = rootfs.exists("usr/etc")?;
    let mut renamed_etc = false;
    if has_usretc {
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
    pub fn undo(&self) -> anyhow::Result<()> {
        if self.renamed_etc {
            /* Remove the symlink and swap back */
            self.rootfs.remove_file("usr/etc")?;
            self.rootfs.local_rename("etc", "usr/etc")?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod test {
    use super::*;
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
