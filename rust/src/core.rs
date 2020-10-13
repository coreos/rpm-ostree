pub use self::ffi::*;
use crate::ffiutil;
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

impl TempEtcGuard {
    /// Create a context with a temporary /etc, and return a guard to it.
    pub fn undo_usretc(rootfs: openat::Dir) -> anyhow::Result<Self> {
        let has_usretc = rootfs.exists("usr/etc")?;
        if has_usretc {
            // In general now, we place contents in /etc when running scripts
            rootfs.local_rename("usr/etc", "etc")?;
            // But leave a compat symlink, as we used to bind mount, so scripts
            // could still use that too.
            rootfs.symlink("usr/etc", "../etc")?;
        }

        let guard = Self {
            rootfs,
            renamed_etc: has_usretc,
        };
        Ok(guard)
    }

    /// Remove the temporary /etc, and destroy the guard.
    pub fn redo_usretc(self) -> anyhow::Result<()> {
        if self.renamed_etc {
            /* Remove the symlink and swap back */
            self.rootfs.remove_file("usr/etc")?;
            self.rootfs.local_rename("etc", "usr/etc")?;
        }
        Ok(())
    }
}

mod ffi {
    use super::*;
    use glib_sys::GError;

    #[no_mangle]
    pub extern "C" fn ror_tempetc_undo_usretc(
        rootfs: libc::c_int,
        gerror: *mut *mut GError,
    ) -> *mut TempEtcGuard {
        let fd = ffiutil::ffi_view_openat_dir(rootfs);
        let res = TempEtcGuard::undo_usretc(fd).map(Box::new);
        ffiutil::ptr_glib_error(res, gerror)
    }

    #[no_mangle]
    pub extern "C" fn ror_tempetc_redo_usretc(
        guard_ptr: *mut TempEtcGuard,
        gerror: *mut *mut GError,
    ) -> libc::c_int {
        assert!(!guard_ptr.is_null());
        let guard = unsafe { Box::from_raw(guard_ptr) };
        let res = guard.redo_usretc();
        ffiutil::int_glib_error(res, gerror)
    }
}
