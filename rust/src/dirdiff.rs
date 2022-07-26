/*
 * Copyright (C) Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//! Compute difference between two filesystem trees.

use anyhow::Context;
use anyhow::Result;
use cap_std::fs::{Dir, Metadata};
use cap_std::io_lifetimes;
use cap_std_ext::cap_std;
use cap_std_ext::dirext::CapStdExtDirExt;
use fn_error_context::context;
use io_lifetimes::AsFilelike;
use serde_derive::{Deserialize, Serialize};
use std::borrow::Cow;
use std::collections::BTreeSet;
use std::fmt;
use std::io::BufReader;

use std::io::Read;
use std::path::Path;

pub(crate) type FileSet = BTreeSet<String>;

/// Diff between two directories.
#[derive(Debug, Default, Serialize, Deserialize)]
pub(crate) struct Diff {
    /// Files that are new in an existing directory
    pub(crate) added_files: FileSet,
    /// New directories
    pub(crate) added_dirs: FileSet,
    /// Files removed
    pub(crate) removed_files: FileSet,
    /// Directories removed (recursively)
    pub(crate) removed_dirs: FileSet,
    /// Files that changed (in any way, metadata or content)
    pub(crate) changed_files: FileSet,
    /// Directories that changed mode/permissions
    pub(crate) changed_dirs: FileSet,
}

impl Diff {
    #[allow(dead_code)]
    pub(crate) fn count(&self) -> usize {
        self.added_files.len()
            + self.added_dirs.len()
            + self.removed_files.len()
            + self.removed_dirs.len()
            + self.changed_files.len()
            + self.changed_dirs.len()
    }

    pub(crate) fn contains(&self, s: &str) -> bool {
        self.added_files.contains(s)
            || self.added_dirs.contains(s)
            || self.removed_files.contains(s)
            || self.removed_dirs.contains(s)
            || self.changed_files.contains(s)
            || self.changed_dirs.contains(s)
    }
}

impl fmt::Display for Diff {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "files(added:{} removed:{} changed:{}) dirs(added:{} removed:{} changed:{})",
            self.added_files.len(),
            self.removed_files.len(),
            self.changed_files.len(),
            self.added_dirs.len(),
            self.removed_dirs.len(),
            self.changed_dirs.len()
        )
    }
}

fn file_content_changed(src: &Dir, dest: &Dir, path: &str, expected_len: u64) -> Result<bool> {
    let mut remaining = expected_len;
    let mut srcf = src.open(path).map(BufReader::new)?;
    let mut destf = dest.open(path).map(BufReader::new)?;
    let mut srcbuf = [0; 4096];
    let mut destbuf = [0; 4096];
    let bufsize = srcbuf.len();
    while remaining > 0 {
        let readlen = std::cmp::min(usize::try_from(remaining).unwrap_or(bufsize), bufsize);
        let srcbuf = &mut srcbuf[0..readlen];
        let destbuf = &mut destbuf[0..readlen];
        srcf.read_exact(srcbuf)?;
        destf.read_exact(destbuf)?;
        if srcbuf != destbuf {
            return Ok(true);
        }
        remaining = remaining
            .checked_sub(readlen as u64)
            .expect("file_content_changed: read underflow");
    }
    Ok(false)
}

fn is_changed(
    src: &Dir,
    dest: &Dir,
    path: &str,
    srcmeta: &Metadata,
    destmeta: &Metadata,
) -> Result<bool> {
    use cap_primitives::fs::read_link_contents;
    if srcmeta.permissions() != destmeta.permissions() {
        return Ok(true);
    }
    let t = srcmeta.file_type();
    let r = if t.is_file() {
        if srcmeta.len() != destmeta.len() {
            true
        } else {
            file_content_changed(src, dest, path, srcmeta.len())
                .context("Comparing file content")?
        }
    } else if t.is_symlink() {
        let src_target = read_link_contents(&src.as_filelike_view(), Path::new(path))?;
        let dest_target = read_link_contents(&dest.as_filelike_view(), Path::new(path))?;
        src_target != dest_target
    } else {
        false
    };
    Ok(r)
}

fn canonicalize_name<'a>(prefix: Option<&str>, name: &'a str) -> Cow<'a, str> {
    if let Some(prefix) = prefix {
        Cow::Owned(format!("{}/{}", prefix, name))
    } else {
        Cow::Borrowed(name)
    }
}

fn diff_recurse(prefix: Option<&str>, src: &Dir, dest: &Dir, diff: &mut Diff) -> Result<()> {
    let list_prefix = prefix.unwrap_or(".");
    for entry in src.read_dir(list_prefix)? {
        let entry = entry?;
        let name = entry.file_name();
        let name = if let Some(name) = name.to_str() {
            name
        } else {
            // For now ignore invalid UTF-8 names
            continue;
        };
        let path = canonicalize_name(prefix, name);
        let pathp = &*path;
        let srctype = entry.file_type()?;
        let is_dir = srctype.is_dir();

        match dest.symlink_metadata_optional(pathp)? {
            Some(destmeta) => {
                let desttype = destmeta.file_type();
                let changed = if srctype != desttype {
                    true
                } else {
                    let srcmeta = src.symlink_metadata(pathp)?;
                    is_changed(src, dest, pathp, &srcmeta, &destmeta)
                        .context("Comparing for changes")?
                };
                if is_dir {
                    diff_recurse(Some(pathp), src, dest, diff)?;
                    if changed {
                        diff.changed_dirs.insert(path.into_owned());
                    }
                } else if changed {
                    diff.changed_files.insert(path.into_owned());
                }
            }
            None => {
                if is_dir {
                    diff.removed_dirs.insert(path.into_owned());
                } else {
                    diff.removed_files.insert(path.into_owned());
                }
            }
        }
    }

    // Iterate over the target (to) directory, and find any
    // files/directories which were not present in the source.
    for entry in dest.read_dir(list_prefix)? {
        let entry = entry?;
        let name = entry.file_name();
        let name = if let Some(name) = name.to_str() {
            name
        } else {
            // For now ignore invalid UTF-8 names
            continue;
        };
        let path = canonicalize_name(prefix, name);
        if src.symlink_metadata_optional(&*path)?.is_some() {
            continue;
        }
        let desttype = entry.file_type()?;
        if desttype.is_dir() {
            diff.added_dirs.insert(path.into_owned());
        } else {
            diff.added_files.insert(path.into_owned());
        }
    }
    Ok(())
}

/// Given two directories, compute the diff between them.
#[context("Computing filesystem diff")]
pub(crate) fn diff(src: &Dir, dest: &Dir) -> Result<Diff> {
    let mut diff = Diff {
        ..Default::default()
    };
    diff_recurse(None, src, dest, &mut diff)?;
    Ok(diff)
}

#[cfg(test)]
mod test {
    use super::*;
    use cap_std::fs::Permissions;
    use std::os::unix::fs::PermissionsExt;

    #[test]
    fn test_diff() -> Result<()> {
        let td = cap_tempfile::tempdir(cap_std::ambient_authority())?;
        td.create_dir("a")?;
        td.create_dir("b")?;
        let a = td.open_dir("a")?;
        let b = td.open_dir("b")?;
        for d in [&a, &b].iter() {
            d.create_dir_all("sub1/sub2")?;
            d.write("sub1/subfile", "subfile")?;
            d.create_dir_all("sub1/sub3")?;
            d.create_dir_all("sub2/sub4")?;
            d.write("sub2/subfile2", "subfile2")?;
            d.write("somefile", "somefile")?;
            cap_primitives::fs::symlink_contents("/", &d.as_filelike_view(), "link2root")?;
            d.symlink("enoent", "brokenlink")?;
            d.symlink("otherlink", "somelink")?;
            d.symlink("sub1/sub2", "otherlink")?;
        }
        // Initial setup with identical dirs
        let d = diff(&a, &b).unwrap();
        assert_eq!(d.count(), 0);

        // Remove a file
        b.remove_file("somefile")?;
        let d = diff(&a, &b).unwrap();
        assert_eq!(d.count(), 1);
        assert_eq!(d.removed_files.len(), 1);

        // Change a file
        b.write("somefile", "somefile2")?;
        let d = diff(&a, &b)?;
        assert_eq!(d.count(), 1);
        assert_eq!(d.changed_files.len(), 1);
        assert!(d.changed_files.contains("somefile"));

        // Many changes
        a.write("sub1/sub2/subfile1", "subfile1")?;
        a.write("sub1/sub2/subfile2", "subfile2")?;
        b.write("sub1/someotherfile", "somefile3")?;
        b.remove_file("link2root")?;
        cap_primitives::fs::symlink_contents("/notroot", &b.as_filelike_view(), "link2root")?;
        b.remove_dir_all("sub2")?;
        a.open("sub1/subfile")?
            .set_permissions(Permissions::from_mode(0o600))?;
        let d = diff(&a, &b)?;
        assert_eq!(d.count(), 7);
        assert_eq!(d.changed_files.len(), 3);
        assert_eq!(d.removed_files.len(), 2);
        assert_eq!(d.added_files.len(), 1);
        assert_eq!(d.removed_dirs.len(), 1);
        assert!(d.removed_files.contains("sub1/sub2/subfile1"));
        assert!(d.added_files.contains("sub1/someotherfile"));
        Ok(())
    }
}
