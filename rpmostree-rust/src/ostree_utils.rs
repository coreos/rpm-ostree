//! Utility helpers or workarounds for incorrectly bound things in ostree-rs
use glib::translate::*;
use std::ptr;

pub(crate) fn sysroot_query_deployments_for(
    sysroot: &ostree::Sysroot,
    osname: &str,
) -> (Option<ostree::Deployment>, Option<ostree::Deployment>) {
    unsafe {
        let mut out_pending = ptr::null_mut();
        let mut out_rollback = ptr::null_mut();
        ostree_sys::ostree_sysroot_query_deployments_for(
            sysroot.to_glib_none().0,
            osname.to_glib_none().0,
            &mut out_pending,
            &mut out_rollback,
        );
        (from_glib_full(out_pending), from_glib_full(out_rollback))
    }
}
