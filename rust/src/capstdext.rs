//! Helper functions for the [`cap-std` crate].
//!
//! [`cap-std` crate]: https://crates.io/crates/cap-std
//  SPDX-License-Identifier: Apache-2.0 OR MIT

use cap_std::fs::DirBuilder;
use cap_std_ext::cap_std;
use cap_std_ext::cap_std::fs::Dir;
use cap_std_ext::rustix;
use std::ffi::{OsStr, OsString};
use std::io::Result;
use std::os::unix::fs::DirBuilderExt;
use std::os::unix::prelude::OsStringExt;
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

pub(crate) fn read_link_contents_impl(dir: &Dir, path: &Path) -> Result<OsString> {
    let parent = path
        .parent()
        .filter(|v| !v.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let dir = dir.open_dir(parent)?;
    let filename = path.file_name().ok_or_else(|| {
        std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "the source path does not name a file",
        )
    })?;
    let l = rustix::fs::readlinkat(&dir, filename, Vec::new())?;
    Ok(OsString::from_vec(l.into_bytes()))
}

// Today cap-std's read_link *also* verifies that the link target doesn't lead outside
// the filesystem.  I think this is arguably incorrect behavior, but for now let's
// add an API that does this.
pub(crate) fn read_link_contents(dir: &Dir, path: impl AsRef<Path>) -> Result<OsString> {
    read_link_contents_impl(dir, path.as_ref())
}

#[cfg(test)]
pub(crate) fn write_link_contents_impl(dir: &Dir, contents: &OsStr, path: &Path) -> Result<()> {
    let parent = path
        .parent()
        .filter(|v| !v.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let dir = dir.open_dir(parent)?;
    let filename = path.file_name().ok_or_else(|| {
        std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "the source path does not name a file",
        )
    })?;
    rustix::fs::symlinkat(contents, &dir, filename).map_err(Into::into)
}

// Today cap-std's read_link *also* verifies that the link target doesn't lead outside
// the filesystem.  I think this is arguably incorrect behavior, but for now let's
// add an API that does this.
#[cfg(test)]
pub(crate) fn write_read_link_contents(
    dir: &Dir,
    original: impl AsRef<OsStr>,
    path: impl AsRef<Path>,
) -> Result<()> {
    write_link_contents_impl(dir, original.as_ref(), path.as_ref())
}
