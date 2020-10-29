use self::ffi::RpmOstreeOrigin;
use std::ptr::NonNull;

/// Reference to an `RpmOstreeOrigin`.
#[derive(Debug)]
pub(crate) struct OriginRef {
    origin: NonNull<RpmOstreeOrigin>,
}

impl OriginRef {
    /// Build a reference object from a C pointer.
    fn from_ffi_ptr(oref: *mut RpmOstreeOrigin) -> Self {
        Self {
            origin: NonNull::new(oref).expect("NULL RpmOstreeOrigin"),
        }
    }

    /// Get `livefs` details for this deployment origin.
    pub(crate) fn get_live_state<'o>(&'o self) -> OriginLiveState<'o> {
        use crate::includes::rpmostree_origin_get_live_state;
        use glib::translate::from_glib_full;

        let mut out_inprogress: *mut libc::c_char = std::ptr::null_mut();
        let mut out_livereplaced: *mut libc::c_char = std::ptr::null_mut();
        unsafe {
            rpmostree_origin_get_live_state(
                self.origin.as_ptr(),
                &mut out_inprogress,
                &mut out_livereplaced,
            );
        };
        let in_progress = unsafe { from_glib_full(out_inprogress) };
        let replaced = unsafe { from_glib_full(out_livereplaced) };

        OriginLiveState {
            _origin: self,
            in_progress,
            replaced,
        }
    }
}

/// `livefs` state and details for a given deployment origin.
#[derive(Debug)]
pub(crate) struct OriginLiveState<'o> {
    /// Underlying deployment origin.
    _origin: &'o OriginRef,
    /// Checksum for the in-progress livefs.
    pub in_progress: Option<String>,
    /// Checksum for the underlying replaced commit.
    pub replaced: Option<String>,
}

impl<'o> OriginLiveState<'o> {
    /// Return whether the given deployment is live-modified.
    pub(crate) fn is_live(self) -> bool {
        self.in_progress.is_some() || self.replaced.is_some()
    }
}

pub mod ffi {
    use super::OriginRef;

    /// Opaque type for C interop: RpmOstreeOrigin.
    pub enum RpmOstreeOrigin {}

    #[no_mangle]
    pub extern "C" fn ror_origin_is_live(origin_ptr: *mut RpmOstreeOrigin) -> libc::c_int {
        let origin = OriginRef::from_ffi_ptr(origin_ptr);
        let livestate = origin.get_live_state();
        livestate.is_live().into()
    }
}
