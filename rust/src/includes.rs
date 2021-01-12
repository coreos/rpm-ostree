/*!
Helper C functions from rpm-ostree.

These are prime candidates for oxidation (e.g. to make interacting with
strings more efficient).

NOTICE: The C header definitions are canonical, please update those first
then synchronize the entries here.
!*/

use crate::passwd::PasswdDB;
use libdnf_sys::DnfPackage;

// From `libpriv/rpmostree-rpm-util.h`.
extern "C" {
    pub(crate) fn rpmostree_get_repodata_chksum_repr(
        package: *mut DnfPackage,
        chksum: *mut *mut libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int;
}

// From `libpriv/rpmostree-passwd-util.h`.
extern "C" {
    #[allow(improper_ctypes)]
    pub(crate) fn rpmostree_add_passwd_to_hash(
        rootfs_dfd: libc::c_int,
        path: *const libc::c_char,
        db: *mut PasswdDB,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int;

    #[allow(improper_ctypes)]
    pub(crate) fn rpmostree_add_group_to_hash(
        rootfs_dfd: libc::c_int,
        path: *const libc::c_char,
        db: *mut PasswdDB,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int;
}
