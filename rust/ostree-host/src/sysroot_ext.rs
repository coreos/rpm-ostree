//! Extension traits that add new APIs.

// SPDX-License-Identifier: Apache-2.0 OR MIT

/// Extension APIs for `ostree::Sysroot`
pub trait SysrootExt {
    /// Require a booted deployment; reimplements https://github.com/ostreedev/ostree/pull/2301
    fn require_booted_deployment(&self) -> Result<ostree::Deployment, glib::Error>;
}

impl SysrootExt for ostree::Sysroot {
    fn require_booted_deployment(&self) -> Result<ostree::Deployment, glib::Error> {
        self.get_booted_deployment().ok_or_else(|| {
            glib::Error::new(gio::IOErrorEnum::Failed, "Not booted into an OSTree system")
        })
    }
}
