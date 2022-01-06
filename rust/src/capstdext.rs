//! Helper functions for the [`cap-std` crate].
//!
//! [`cap-std` crate]: https://crates.io/crates/cap-std
//  SPDX-License-Identifier: Apache-2.0 OR MIT

use std::io::Result;
use std::os::unix::fs::DirBuilderExt;
use std::os::unix::prelude::PermissionsExt;
use std::path::Path;

use cap_std::fs::DirBuilder;

pub(crate) trait CapStdDirExt {
    /// Create the target directory, but do nothing if it already exists.
    fn ensure_dir_with(
        &self,
        p: impl AsRef<Path>,
        builder: &cap_std::fs::DirBuilder,
    ) -> Result<bool>;
}

impl CapStdDirExt for cap_std::fs::Dir {
    fn ensure_dir_with(
        &self,
        p: impl AsRef<Path>,
        builder: &cap_std::fs::DirBuilder,
    ) -> Result<bool> {
        match self.create_dir_with(p, builder) {
            Ok(()) => Ok(false),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => Ok(true),
            Err(e) => Err(e),
        }
    }
}

pub(crate) fn dirbuilder_from_mode(m: u32) -> DirBuilder {
    let mut r = DirBuilder::new();
    r.mode(m);
    r
}

pub(crate) fn perms_from_mode(m: u32) -> cap_std::fs::Permissions {
    cap_std::fs::Permissions::from_std(std::fs::Permissions::from_mode(m))
}
