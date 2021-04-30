//! Logic for unpacking and importing an RPM.
//!
//! The design here is to reuse libarchive's RPM support for most of it.
//! We do however need to look at file capabilities, which are part of the header.
//! Hence we end up with two file descriptors open.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::{CxxResult, FFIGObjectWrapper};
use anyhow::{bail, Result};
use fn_error_context::context;
use gio::FileType;
use ostree::RepoCommitFilterResult;
use std::pin::Pin;

/// Adjust mode for specific file entries.
pub fn tweak_imported_file_info(
    mut file_info: Pin<&mut crate::FFIGFileInfo>,
    ro_executables: bool,
) {
    let file_info = file_info.gobj_wrap();
    let filetype = file_info.get_file_type();

    // Add the "user writable" permission bit for a directory.
    // See this bug:
    // https://bugzilla.redhat.com/show_bug.cgi?id=517575
    if filetype == FileType::Directory {
        let mut mode = file_info.get_attribute_uint32("unix::mode");
        mode |= libc::S_IWUSR;
        file_info.set_attribute_uint32("unix::mode", mode);
    }

    // Ensure executable files are not writable.
    // See similar code in `ostree commit`:
    // https://github.com/ostreedev/ostree/pull/2091/commits/7392259332e00c33ed45b904deabde08f4da3e3c
    if ro_executables && filetype == FileType::Regular {
        let mut mode = file_info.get_attribute_uint32("unix::mode");
        if (mode & (libc::S_IXUSR | libc::S_IXGRP | libc::S_IXOTH)) != 0 {
            mode &= !(libc::S_IWUSR | libc::S_IWGRP | libc::S_IWOTH);
            file_info.set_attribute_uint32("unix::mode", mode);
        }
    }
}

/// Apply filtering and manipulation logic to an RPM file before importing.
///
/// This returns whether the entry should be ignored by the importer.
pub fn importer_compose_filter(
    path: &str,
    mut file_info: Pin<&mut crate::FFIGFileInfo>,
    skip_extraneous: bool,
) -> CxxResult<bool> {
    let mut file_info = file_info.gobj_wrap();
    match import_filter(path, &mut file_info, skip_extraneous)? {
        RepoCommitFilterResult::Allow => Ok(false),
        RepoCommitFilterResult::Skip => Ok(true),
        x => unreachable!("unknown commit result '{}' for path '{}'", x, path),
    }
}

#[context("Analyzing {}", path)]
fn import_filter(
    path: &str,
    file_info: &mut gio::FileInfo,
    skip_extraneous: bool,
) -> Result<RepoCommitFilterResult> {
    // Sanity check that RPM isn't using CPIO id fields.
    {
        let uid = file_info.get_attribute_uint32("unix::uid");
        let gid = file_info.get_attribute_uint32("unix::gid");
        if uid != 0 || gid != 0 {
            bail!("Unexpected non-root owned path (marked as {}:{})", uid, gid);
        }
    }

    // Skip some empty lock files, they are known to cause problems:
    // https://github.com/projectatomic/rpm-ostree/pull/1002
    if path.starts_with("/usr/etc/selinux") && path.ends_with(".LOCK") {
        return Ok(RepoCommitFilterResult::Skip);
    }

    // /run and /var are directly converted to tmpfiles.d fragments elsewhere.
    if path.starts_with("/run") || path.starts_with("/var") {
        return Ok(RepoCommitFilterResult::Skip);
    }

    // And ensure the RPM installs into supported paths.
    // Note that we rewrite /opt in `handle_translate_pathname`, but
    // this gets called with the old path, so handle it here too.
    let is_supported = path_is_ostree_compliant(path) || path_is_in_opt(path);
    if !is_supported {
        if !skip_extraneous {
            bail!("Unsupported path; see https://github.com/projectatomic/rpm-ostree/issues/233");
        }
        return Ok(RepoCommitFilterResult::Skip);
    }

    Ok(RepoCommitFilterResult::Allow)
}

/// Whether absolute `path` is allowed in OSTree content.
///
/// When we do a unified core, we'll likely need to add /boot to pick up
/// kernels here at least.  This is intended short term to address
/// https://github.com/projectatomic/rpm-ostree/issues/233
fn path_is_ostree_compliant(path: &str) -> bool {
    if matches!(path, "/" | "/usr" | "/bin" | "/sbin" | "/lib" | "/lib64") {
        return true;
    }

    if path.starts_with("/bin/")
        || path.starts_with("/sbin/")
        || path.starts_with("/lib/")
        || path.starts_with("/lib64/")
    {
        return true;
    }

    if path.starts_with("/usr/") && !path.starts_with("/usr/local") {
        return true;
    }

    false
}

/// Whether absolute `path` belongs to `/opt` hierarchy.
fn path_is_in_opt(path: &str) -> bool {
    path == "/opt" || path.starts_with("/opt/")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_path_is_compliant() {
        let ostree_cases = &["/", "/usr", "/usr/share", "/bin/foo"];
        for entry in ostree_cases {
            assert_eq!(path_is_ostree_compliant(entry), true, "{}", entry);
            assert_eq!(path_is_in_opt(entry), false, "{}", entry);
        }

        let opt_cases = &["/opt", "/opt/misc"];
        for entry in opt_cases {
            assert_eq!(path_is_ostree_compliant(entry), false, "{}", entry);
            assert_eq!(path_is_in_opt(entry), true, "{}", entry);
        }

        let denied_cases = &["/var", "/etc", "/var/run", "/usr/local", "", "./", "usr/"];
        for entry in denied_cases {
            assert_eq!(path_is_ostree_compliant(entry), false, "{}", entry);
            assert_eq!(path_is_in_opt(entry), false, "{}", entry);
        }
    }
}
