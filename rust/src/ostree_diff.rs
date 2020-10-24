/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use anyhow::{Context, Result};
use gio::prelude::*;
use ostree::RepoFileExt;
use serde_derive::{Deserialize, Serialize};
use std::collections::BTreeSet;

/// Like `g_file_query_info()`, but return None if the target doesn't exist.
fn query_info_optional(
    f: &gio::File,
    queryattrs: &str,
    queryflags: gio::FileQueryInfoFlags,
) -> Result<Option<gio::FileInfo>> {
    let cancellable = gio::NONE_CANCELLABLE;
    match f.query_info(queryattrs, queryflags, cancellable) {
        Ok(i) => Ok(Some(i)),
        Err(e) => {
            if let Some(ref e2) = e.kind::<gio::IOErrorEnum>() {
                match e2 {
                    gio::IOErrorEnum::NotFound => Ok(None),
                    _ => return Err(e.into()),
                }
            } else {
                return Err(e.into());
            }
        }
    }
}

pub(crate) type FileSet = BTreeSet<String>;

/// Diff between two ostree commits.
#[derive(Debug, Default, Serialize, Deserialize)]
pub(crate) struct FileTreeDiff {
    /// The prefix passed for diffing, e.g. /usr
    pub(crate) subdir: Option<String>,
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

fn diff_recurse(
    prefix: &str,
    diff: &mut FileTreeDiff,
    from: &ostree::RepoFile,
    to: &ostree::RepoFile,
) -> Result<()> {
    let cancellable = gio::NONE_CANCELLABLE;
    let queryattrs = "standard::name,standard::type";
    let queryflags = gio::FileQueryInfoFlags::NOFOLLOW_SYMLINKS;
    let from_iter = from.enumerate_children(queryattrs, queryflags, cancellable)?;

    // Iterate over the source (from) directory, and compare with the
    // target (to) directory.  This generates removals and changes.
    while let Some(from_info) = from_iter.next_file(cancellable)? {
        let from_child = from_iter.get_child(&from_info).expect("file");
        let name = from_info.get_name().expect("name");
        let name = name.to_str().expect("UTF-8 ostree name");
        let path = format!("{}{}", prefix, name);
        let to_child = to.get_child(&name).expect("child");
        let to_info = query_info_optional(&to_child, queryattrs, queryflags)
            .context("querying optional to")?;
        let is_dir = match from_info.get_file_type() {
            gio::FileType::Directory => true,
            _ => false,
        };
        if to_info.is_some() {
            let to_child = to_child.downcast::<ostree::RepoFile>().expect("downcast");
            to_child.ensure_resolved()?;
            let from_child = from_child.downcast::<ostree::RepoFile>().expect("downcast");
            from_child.ensure_resolved()?;

            if is_dir {
                let from_contents_checksum =
                    from_child.tree_get_contents_checksum().expect("checksum");
                let to_contents_checksum = to_child.tree_get_contents_checksum().expect("checksum");
                if from_contents_checksum != to_contents_checksum {
                    let subpath = format!("{}/", path);
                    diff_recurse(&subpath, diff, &from_child, &to_child)?;
                }
                let from_meta_checksum = from_child.tree_get_metadata_checksum().expect("checksum");
                let to_meta_checksum = to_child.tree_get_metadata_checksum().expect("checksum");
                if from_meta_checksum != to_meta_checksum {
                    diff.changed_dirs.insert(path);
                }
            } else {
                let from_checksum = from_child.get_checksum().expect("checksum");
                let to_checksum = to_child.get_checksum().expect("checksum");
                if from_checksum != to_checksum {
                    diff.changed_files.insert(path);
                }
            }
        } else {
            if is_dir {
                diff.removed_dirs.insert(path);
            } else {
                diff.removed_files.insert(path);
            }
        }
    }
    // Iterate over the target (to) directory, and find any
    // files/directories which were not present in the source.
    let to_iter = to.enumerate_children(queryattrs, queryflags, cancellable)?;
    while let Some(to_info) = to_iter.next_file(cancellable)? {
        let name = to_info.get_name().expect("name");
        let name = name.to_str().expect("UTF-8 ostree name");
        let path = format!("{}{}", prefix, name);
        let from_child = from.get_child(name).expect("child");
        let from_info = query_info_optional(&from_child, queryattrs, queryflags)
            .context("querying optional from")?;
        if from_info.is_some() {
            continue;
        }
        let is_dir = match to_info.get_file_type() {
            gio::FileType::Directory => true,
            _ => false,
        };
        if is_dir {
            diff.added_dirs.insert(path);
        } else {
            diff.added_files.insert(path);
        }
    }
    Ok(())
}

/// Given two ostree commits, compute the diff between them.
pub(crate) fn diff<P: AsRef<str>>(
    repo: &ostree::Repo,
    from: &str,
    to: &str,
    subdir: Option<P>,
) -> Result<FileTreeDiff> {
    let subdir = subdir.as_ref();
    let subdir = subdir.map(|s| s.as_ref());
    let (fromroot, _) = repo.read_commit(from, gio::NONE_CANCELLABLE)?;
    let (toroot, _) = repo.read_commit(to, gio::NONE_CANCELLABLE)?;
    let (fromroot, toroot) = if let Some(subdir) = subdir {
        (
            fromroot.resolve_relative_path(subdir).expect("path"),
            toroot.resolve_relative_path(subdir).expect("path"),
        )
    } else {
        (fromroot, toroot)
    };
    let fromroot = fromroot.downcast::<ostree::RepoFile>().expect("downcast");
    fromroot.ensure_resolved()?;
    let toroot = toroot.downcast::<ostree::RepoFile>().expect("downcast");
    toroot.ensure_resolved()?;
    let mut diff = FileTreeDiff {
        subdir: subdir.map(|s| s.to_string()),
        ..Default::default()
    };
    diff_recurse("/", &mut diff, &fromroot, &toroot)?;
    Ok(diff)
}
