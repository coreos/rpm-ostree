//! Extension traits fixing incorrectly bound things in ostree-rs
//! by defining a new function with an `x_` prefix.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use glib::translate::*;
use std::ptr;

/// Extension functions which fix incorrectly bound APIs.
pub trait RepoBindingExt {
    fn x_resolve_ref_optional(&self, refspec: &str) -> Result<Option<glib::GString>, glib::Error>;
}

impl RepoBindingExt for ostree::Repo {
    fn x_resolve_ref_optional(&self, refspec: &str) -> Result<Option<glib::GString>, glib::Error> {
        unsafe {
            let mut out_rev = ptr::null_mut();
            let mut error = ptr::null_mut();
            let refspec = refspec.to_glib_none();
            let _ = ostree_sys::ostree_repo_resolve_rev(
                self.to_glib_none().0,
                refspec.0,
                true.to_glib(),
                &mut out_rev,
                &mut error,
            );
            if error.is_null() {
                Ok(from_glib_full(out_rev))
            } else {
                Err(from_glib_full(error))
            }
        }
    }
}

/// Extension functions which fix incorectly bound APIs
pub trait SysrootBindingExt {
    fn x_query_deployments_for(
        &self,
        osname: &str,
    ) -> (Option<ostree::Deployment>, Option<ostree::Deployment>);
}

impl SysrootBindingExt for ostree::Sysroot {
    fn x_query_deployments_for(
        &self,
        osname: &str,
    ) -> (Option<ostree::Deployment>, Option<ostree::Deployment>) {
        unsafe {
            let mut out_pending = ptr::null_mut();
            let mut out_rollback = ptr::null_mut();
            ostree_sys::ostree_sysroot_query_deployments_for(
                self.to_glib_none().0,
                osname.to_glib_none().0,
                &mut out_pending,
                &mut out_rollback,
            );
            (from_glib_full(out_pending), from_glib_full(out_rollback))
        }
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use anyhow::Result;

    #[test]
    fn optional_ref() -> Result<()> {
        let cancellable = gio::NONE_CANCELLABLE;
        let td = tempfile::tempdir()?;
        let r = ostree::Repo::new_for_path(td.path());
        r.create(ostree::RepoMode::Archive, cancellable)?;
        assert!(r.x_resolve_ref_optional("someref")?.is_none());
        Ok(())
    }
}
