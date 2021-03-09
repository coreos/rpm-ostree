/*
 * Copyright (C) Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//! Compute difference between two filesystem trees.

use anyhow::Result;
use fn_error_context::context;
use openat_ext::OpenatDirExt;
use serde_derive::{Deserialize, Serialize};
use std::borrow::Cow;
use std::collections::BTreeSet;
use std::convert::TryFrom;
use std::fmt;

use std::io::Read;

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

fn file_content_changed(
    src: &openat::Dir,
    dest: &openat::Dir,
    path: &str,
    expected_len: u64,
) -> Result<bool> {
    let mut remaining = expected_len;
    let mut srcf = std::io::BufReader::new(src.open_file(path)?);
    let mut destf = std::io::BufReader::new(dest.open_file(path)?);
    let mut srcbuf = [0; 4096];
    let mut destbuf = [0; 4096];
    let bufsize = srcbuf.len();
    while remaining > 0 {
        let readlen = std::cmp::min(usize::try_from(remaining).unwrap_or(bufsize), bufsize);
        let mut srcbuf = &mut srcbuf[0..readlen];
        let mut destbuf = &mut destbuf[0..readlen];
        srcf.read_exact(&mut srcbuf)?;
        destf.read_exact(&mut destbuf)?;
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
    src: &openat::Dir,
    dest: &openat::Dir,
    path: &str,
    srcmeta: &openat::Metadata,
    destmeta: &openat::Metadata,
) -> Result<bool> {
    if srcmeta.permissions() != destmeta.permissions() {
        return Ok(true);
    }
    Ok(match srcmeta.simple_type() {
        openat::SimpleType::File => {
            if srcmeta.len() != destmeta.len() {
                true
            } else {
                file_content_changed(src, dest, path, srcmeta.len())?
            }
        }
        openat::SimpleType::Symlink => src.read_link(path)? != dest.read_link(path)?,
        openat::SimpleType::Other => false,
        openat::SimpleType::Dir => false,
    })
}

fn canonicalize_name<'a>(prefix: Option<&str>, name: &'a str) -> Cow<'a, str> {
    if let Some(prefix) = prefix {
        Cow::Owned(format!("{}/{}", prefix, name))
    } else {
        Cow::Borrowed(name)
    }
}

fn diff_recurse(
    prefix: Option<&str>,
    src: &openat::Dir,
    dest: &openat::Dir,
    diff: &mut Diff,
) -> Result<()> {
    let list_prefix = prefix.unwrap_or(".");
    for entry in src.list_dir(list_prefix)? {
        let entry = entry?;
        let name = if let Some(name) = entry.file_name().to_str() {
            name
        } else {
            // For now ignore invalid UTF-8 names
            continue;
        };
        let path = canonicalize_name(prefix, name);
        let pathp = &*path;
        let srctype = src.get_file_type(&entry)?;
        let is_dir = srctype == openat::SimpleType::Dir;

        match dest.metadata_optional(pathp)? {
            Some(destmeta) => {
                let desttype = destmeta.simple_type();
                let changed = if srctype != desttype {
                    true
                } else {
                    let srcmeta = src.metadata(pathp)?;
                    is_changed(src, dest, pathp, &srcmeta, &destmeta)?
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
    for entry in dest.list_dir(list_prefix)? {
        let entry = entry?;
        let name = if let Some(name) = entry.file_name().to_str() {
            name
        } else {
            // For now ignore invalid UTF-8 names
            continue;
        };
        let path = canonicalize_name(prefix, name);
        if src.metadata_optional(&*path)?.is_some() {
            continue;
        }
        let desttype = dest.get_file_type(&entry)?;
        if desttype == openat::SimpleType::Dir {
            diff.added_dirs.insert(path.into_owned());
        } else {
            diff.added_files.insert(path.into_owned());
        }
    }
    Ok(())
}

/// Given two directories, compute the diff between them.
#[context("Computing filesystem diff")]
pub(crate) fn diff(src: &openat::Dir, dest: &openat::Dir) -> Result<Diff> {
    let mut diff = Diff {
        ..Default::default()
    };
    diff_recurse(None, src, dest, &mut diff)?;
    Ok(diff)
}

#[cfg(test)]
mod test {
    use super::*;
    use std::fs::Permissions;
    use std::os::unix::fs::PermissionsExt;

    #[test]
    fn test_diff() -> Result<()> {
        let td = tempfile::tempdir()?;
        let td = openat::Dir::open(td.path())?;
        td.create_dir("a", 0o755)?;
        td.create_dir("b", 0o755)?;
        let a = td.sub_dir("a")?;
        let b = td.sub_dir("b")?;
        for d in [&a, &b].iter() {
            d.ensure_dir_all("sub1/sub2", 0o755)?;
            d.write_file_contents("sub1/subfile", 0o644, "subfile")?;
            d.ensure_dir_all("sub1/sub3", 0o755)?;
            d.ensure_dir_all("sub2/sub4", 0o755)?;
            d.write_file_contents("sub2/subfile2", 0o644, "subfile2")?;
            d.write_file_contents("somefile", 0o644, "somefile")?;
            d.symlink("link2root", "/")?;
            d.symlink("brokenlink", "enoent")?;
            d.symlink("somelink", "otherlink")?;
            d.symlink("otherlink", "sub1/sub2")?;
        }
        // Initial setup with identical dirs
        let d = diff(&a, &b)?;
        assert_eq!(d.count(), 0);

        // Remove a file
        b.remove_file("somefile")?;
        let d = diff(&a, &b)?;
        assert_eq!(d.count(), 1);
        assert_eq!(d.removed_files.len(), 1);

        // Change a file
        b.write_file_contents("somefile", 0o644, "somefile2")?;
        let d = diff(&a, &b)?;
        assert_eq!(d.count(), 1);
        assert_eq!(d.changed_files.len(), 1);
        assert!(d.changed_files.contains("somefile"));

        // Many changes
        a.write_file_contents("sub1/sub2/subfile1", 0o644, "subfile1")?;
        a.write_file_contents("sub1/sub2/subfile2", 0o644, "subfile2")?;
        b.write_file_contents("sub1/someotherfile", 0o644, "somefile3")?;
        b.remove_file("link2root")?;
        b.symlink("link2root", "/notroot")?;
        b.remove_all("sub2")?;
        a.open_file("sub1/subfile")?
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
