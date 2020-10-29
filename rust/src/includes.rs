/*!
Helper C functions from rpm-ostree.

These are prime candidates for oxidation (e.g. to make interacting with
strings more efficient).

NOTICE: The C header definitions are canonical, please update those first
then synchronize the entries here.
!*/

use crate::syscore::ffi::RpmOstreeOrigin;
use libdnf_sys::DnfPackage;

// From `libpriv/rpmostree-rpm-util.h`.
extern "C" {
    pub(crate) fn rpmostree_get_repodata_chksum_repr(
        package: *mut DnfPackage,
        chksum: *mut *mut libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int;
}

// From `libpriv/rpmostree-origin.h`.
extern "C" {
    pub(crate) fn rpmostree_origin_get_live_state(
        origin: *mut RpmOstreeOrigin,
        out_inprogress: *mut *mut libc::c_char,
        out_livereplaced: *mut *mut libc::c_char,
    );
}
