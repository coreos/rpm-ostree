//! Helpers for working with an ostree filesystem

// SPDX-License-Identifier: Apache-2.0 OR MIT

use camino::{Utf8Path, Utf8PathBuf};
use ostree_ext::{gio, ostree, prelude::*};
use std::{collections::HashMap, path::Component};

#[derive(Debug, Clone)]
pub struct ResolvedOstreePaths {
    pub path: ostree::RepoFile,
    pub symlink_target: Option<ostree::RepoFile>,
}

impl ResolvedOstreePaths {
    /// If the resolved file is itself a symlink, returns the
    /// file it points to. Otherwise just returns the resolved file.
    pub fn real_file(&self) -> &ostree::RepoFile {
        self.symlink_target.as_ref().unwrap_or(&self.path)
    }
}

/// Given a path and an ostree filesystem root, resolves
/// the path to a real file on the filesystem, including
/// resolving symlinks in the path's directory tree.
///
/// Returns a pair of the resolved path and in the case where
/// the path points to a symlink, it also includes the resolved
/// symlink target.
pub fn resolve_ostree_paths(
    path: &Utf8Path,
    fsroot: &ostree::RepoFile,
    cache: &mut HashMap<Utf8PathBuf, ResolvedOstreePaths>,
) -> Option<ResolvedOstreePaths> {
    assert!(path.is_absolute());

    // Recurse until root
    if path.parent().is_none() {
        return Some(ResolvedOstreePaths {
            path: fsroot.clone(),
            symlink_target: None,
        });
    }

    // Check the cache for this path
    if let Some(cache_hit) = cache.get(path) {
        return Some(cache_hit.clone());
    }

    // Resolve our parent
    let parent = resolve_ostree_paths(path.parent().unwrap(), fsroot, cache)?;

    // Resolve ourselves from our parent
    let child_file = parent
        .real_file()
        .child(path.file_name().unwrap())
        .downcast::<ostree::RepoFile>()
        .unwrap();

    if !child_file.query_exists(gio::Cancellable::NONE) {
        return None;
    }

    let child_info = child_file
        .query_info("*", gio::FileQueryInfoFlags::NONE, gio::Cancellable::NONE)
        .expect("failed to get fs info");

    // If this path is a symlink, figure out what it points to
    let remapped_target =
        if child_info.has_attribute("standard::is-symlink") && child_info.is_symlink() {
            let link_target = child_info.symlink_target().unwrap();

            // Due to a bug in OSTree's Gio.File implementation, we cannot
            // just do `parent.resolve_relative_path` here as it doesn't correctly
            // resolve to an absolute path.
            // So instead we'll handle '.' and '..' chunks ourselves and normalize the
            // resulting path.
            if !link_target.is_absolute() {
                let mut target_path = path.parent().unwrap().to_owned();

                for item in link_target.components() {
                    match item {
                        Component::ParentDir => {
                            target_path.pop();
                        }
                        Component::Normal(name) => {
                            target_path.push(Utf8Path::new(&name.to_string_lossy()));
                        }
                        Component::CurDir => {}
                        _ => panic!("unhandled component type"),
                    };
                }

                resolve_ostree_paths(&target_path, fsroot, cache)
            } else {
                resolve_ostree_paths(Utf8Path::from_path(&link_target).unwrap(), fsroot, cache)
            }
        } else {
            None
        };

    let result = ResolvedOstreePaths {
        path: child_file.clone(),
        symlink_target: remapped_target.map(|f| f.real_file().clone()),
    };

    // If this path is or points to a directory, add it to the cache to speed up future lookups
    if result.real_file().is_dir() {
        cache.insert(path.to_owned(), result.clone());
    }

    Some(result)
}

pub trait FileHelpers {
    fn is_dir(&self) -> bool;
    fn is_regular(&self) -> bool;
    fn is_symlink(&self) -> bool;
}

impl<T> FileHelpers for T
where
    T: IsA<gio::File>,
{
    fn is_dir(&self) -> bool {
        self.query_file_type(gio::FileQueryInfoFlags::NONE, gio::Cancellable::NONE)
            == gio::FileType::SymbolicLink
    }

    fn is_regular(&self) -> bool {
        self.query_file_type(gio::FileQueryInfoFlags::NONE, gio::Cancellable::NONE)
            == gio::FileType::Regular
    }

    fn is_symlink(&self) -> bool {
        self.query_file_type(gio::FileQueryInfoFlags::NONE, gio::Cancellable::NONE)
            == gio::FileType::SymbolicLink
    }
}
