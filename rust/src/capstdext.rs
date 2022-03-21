//! Helper functions for the [`cap-std` crate].
//!
//! [`cap-std` crate]: https://crates.io/crates/cap-std
//  SPDX-License-Identifier: Apache-2.0 OR MIT

use cap_std::fs::DirBuilder;
use cap_std_ext::cap_std;
use cap_std_ext::cap_std::fs::Dir;
use std::ffi::OsStr;
use std::io::Result;
use std::os::unix::fs::DirBuilderExt;
use std::path::Path;

pub(crate) fn dirbuilder_from_mode(m: u32) -> DirBuilder {
    let mut r = DirBuilder::new();
    r.mode(m);
    r
}

/// Given a (possibly absolute) filename, return its parent directory and filename.
pub(crate) fn open_dir_of(
    path: &Path,
    ambient_authority: cap_std::AmbientAuthority,
) -> Result<(Dir, &OsStr)> {
    let parent = path
        .parent()
        .filter(|v| !v.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let parent = Dir::open_ambient_dir(parent, ambient_authority)?;
    let filename = path.file_name().ok_or_else(|| {
        std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "the source path does not name a file",
        )
    })?;
    Ok((parent, filename))
}
