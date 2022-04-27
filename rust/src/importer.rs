//! Logic for unpacking and importing an RPM.
//!
//! The design here is to reuse libarchive's RPM support for most of it.
//! We do however need to look at file capabilities, which are part of the header.
//! Hence we end up with two file descriptors open.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::{CxxResult, FFIGObjectWrapper};
use crate::utils;
use anyhow::{bail, format_err, Result};
use camino::{Utf8Path, Utf8PathBuf};
use fn_error_context::context;
use gio::{FileInfo, FileType};
use ostree::RepoCommitFilterResult;
use ostree_ext::{gio, ostree};
use std::borrow::Cow;
use std::collections::BTreeSet;
use std::fmt::Write;
use std::pin::Pin;

#[derive(Debug)]
pub struct RpmImporter {
    // Hashset of filepath entries which are direct children of /opt;
    // each key is a plain path fragment, e.g. 'foo' for '/opt/foo/bar'.
    opt_direntries: BTreeSet<String>,
    /// Set of directories which got moved from '/var/lib/' to '/usr/lib/';
    /// each key is a plain directory name, e.g. 'foo' for '/var/lib/foo/'.
    varlib_direntries: BTreeSet<String>,
}

pub fn rpm_importer_new() -> Box<RpmImporter> {
    Box::new(RpmImporter::new())
}

impl RpmImporter {
    pub(crate) fn new() -> Self {
        Self {
            opt_direntries: BTreeSet::new(),
            varlib_direntries: BTreeSet::new(),
        }
    }

    fn get_first_path_element(rel_path: &str) -> String {
        match rel_path.split_once("/") {
            Some((dirname, _rest)) => dirname.to_string(),
            None => rel_path.to_string(),
        }
    }

    /// Callback for ostree importer `translate_pathname`.
    pub fn handle_translate_pathname(&mut self, path: &str) -> String {
        self.inspect_path_for_symlink_translation(path);
        utils::translate_path_for_ostree(path)
    }

    /// Process special paths which need symlink translation.
    ///
    /// This detects cases where an RPM does ship content under `/opt` or `/var`.
    /// Those paths are translated back under `/usr`, and a compatibility symlink
    /// is created through `systemd-tmpfiles`.
    pub fn inspect_path_for_symlink_translation(&mut self, path: &str) -> bool {
        let special_handlers = [Self::inspect_opt_path, Self::inspect_varlib_path];
        special_handlers.iter().any(|handler| handler(self, path))
    }

    /// Inspect a given path for special `opt/<foo>` entries.
    ///
    /// This returns whether the input path matched one of the special
    /// cases.
    fn inspect_opt_path(&mut self, path: &str) -> bool {
        if let Some(prefixless) = path.strip_prefix("opt/") {
            let dirname = Self::get_first_path_element(prefixless);
            if !dirname.is_empty() {
                self.opt_direntries.insert(dirname);
                return true;
            }
        }
        false
    }

    /// Inspect a given path for special `var/lib/<foo>` entries.
    ///
    /// This returns whether the input path matched one of the special
    /// cases.
    fn inspect_varlib_path(&mut self, path: &str) -> bool {
        let dirname = match path {
            "var/lib/alternatives" => Some("alternatives".to_string()),
            "var/lib/vagrant" => Some("vagrant".to_string()),
            _ => None,
        };

        if let Some(entry) = dirname {
            self.varlib_direntries.insert(entry);
            true
        } else {
            false
        }
    }

    /// Format tmpfiles.d lines for symlinked entries.
    // NOTE(lucab): destinations (dirname) can't be quoted as systemd just
    // parses the remainder of the line, and doesn't expand quotes.
    pub fn tmpfiles_symlink_entries(&self) -> Vec<String> {
        // /opt/ symlinks
        let opt_entries = self.opt_direntries.iter().map(|dirname| {
            let quoted = crate::maybe_shell_quote(&format!("/opt/{dirname}"));
            format!("L {quoted} - - - - ../../usr/lib/opt/{dirname}")
        });

        // /var/lib/ symlinks
        let varlib_entries = self.varlib_direntries.iter().map(|dirname| {
            let quoted = crate::maybe_shell_quote(&format!("/var/lib/{dirname}"));
            format!("L {quoted} - - - - ../../usr/lib/{dirname}")
        });

        opt_entries.chain(varlib_entries).collect()
    }
}

/// Canonicalize a path, e.g. replace `//` with `/` and `././` with `./`.
// For some background behind this, see https://github.com/alexcrichton/tar-rs/pull/274
// The specific problem case was:
// # rpm -qf /usr/lib/systemd/systemd-sysv-install
// chkconfig-1.13-2.el8.x86_64
// # ll /usr/lib/systemd/systemd-sysv-install
// lrwxrwxrwx. 2 root root 24 Nov 29 18:08 /usr/lib/systemd/systemd-sysv-install -> ../../..//sbin/chkconfig
// #
fn canonicalize_path(p: &str) -> String {
    let p = Utf8Path::new(p);
    let mut r = Utf8PathBuf::new();
    for part in p.components() {
        r.push(part);
    }
    r.into_string()
}

/// Adjust mode for specific file entries.
pub fn tweak_imported_file_info(
    mut file_info: Pin<&mut crate::FFIGFileInfo>,
    ro_executables: bool,
) {
    let file_info = file_info.gobj_wrap();
    let filetype = file_info.file_type();

    // Add the "user writable" permission bit for a directory.
    // See this bug:
    // https://bugzilla.redhat.com/show_bug.cgi?id=517575
    if filetype == FileType::Directory {
        let mut mode = file_info.attribute_uint32("unix::mode");
        mode |= libc::S_IWUSR;
        file_info.set_attribute_uint32("unix::mode", mode);
    }

    // Ensure executable files are not writable.
    // See similar code in `ostree commit`:
    // https://github.com/ostreedev/ostree/pull/2091/commits/7392259332e00c33ed45b904deabde08f4da3e3c
    if ro_executables && filetype == FileType::Regular {
        let mut mode = file_info.attribute_uint32("unix::mode");
        if (mode & (libc::S_IXUSR | libc::S_IXGRP | libc::S_IXOTH)) != 0 {
            mode &= !(libc::S_IWUSR | libc::S_IWGRP | libc::S_IWOTH);
            file_info.set_attribute_uint32("unix::mode", mode);
        }
    }

    if filetype == FileType::SymbolicLink {
        if let Some(target) = file_info.symlink_target() {
            // See above, this is a special case hack until
            // https://github.com/fedora-sysv/chkconfig/pull/67 propagates everywhere
            // and/or https://github.com/ostreedev/ostree-rs-ext/pull/182 merges.
            if target.ends_with("//sbin/chkconfig") {
                let canonicalized = &canonicalize_path(&target);
                file_info.set_symlink_target(canonicalized);
            }
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
    file_info: &mut FileInfo,
    skip_extraneous: bool,
) -> Result<RepoCommitFilterResult> {
    // Sanity check that RPM isn't using CPIO id fields.
    let uid = file_info.attribute_uint32("unix::uid");
    let gid = file_info.attribute_uint32("unix::gid");
    if uid != 0 || gid != 0 {
        bail!("Unexpected non-root owned path (marked as {}:{})", uid, gid);
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
    // Note that we rewrote /opt to /usr/lib/opt in `handle_translate_pathname`.
    if !path_is_ostree_compliant(path) {
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

pub fn tmpfiles_translate(
    abs_path: &str,
    mut file_info: Pin<&mut crate::FFIGFileInfo>,
    username: &str,
    groupname: &str,
) -> CxxResult<String> {
    let file_info = file_info.gobj_wrap();
    let entry = translate_to_tmpfiles_d(abs_path, &file_info, username, groupname)?;
    Ok(entry)
}

/// Translate a filepath entry to an equivalent tmpfiles.d line.
#[context("Translating {}", abs_path)]
pub(crate) fn translate_to_tmpfiles_d(
    abs_path: &str,
    file_info: &FileInfo,
    username: &str,
    groupname: &str,
) -> Result<String> {
    let mut bufwr = String::new();

    let path_type = file_info.file_type();
    let filetype_char = match path_type {
        FileType::Directory => 'd',
        FileType::Regular => 'f',
        FileType::SymbolicLink => 'L',
        x => bail!("path '{}' has invalid type: {:?}", abs_path, x),
    };
    let fixed_path = fix_tmpfiles_path(Cow::Borrowed(abs_path));
    write!(&mut bufwr, "{} {}", filetype_char, fixed_path)?;

    if path_type == FileType::SymbolicLink {
        let link_target = file_info
            .symlink_target()
            .ok_or_else(|| format_err!("missing symlink target"))?;
        write!(&mut bufwr, " - - - - {}", link_target)?;
    } else {
        let mode = file_info.attribute_uint32("unix::mode") & !libc::S_IFMT;
        write!(&mut bufwr, " {:04o} {} {} - -", mode, username, groupname)?;
    };

    Ok(bufwr)
}

fn fix_tmpfiles_path(abs_path: Cow<str>) -> Cow<str> {
    let mut tweaked_path = abs_path;

    // systemd-tmpfiles complains loudly about writing to /var/run;
    // ideally, all of the packages get fixed for this but...eh.
    if tweaked_path.starts_with("/var/run/") {
        let trimmed = tweaked_path.trim_start_matches("/var");
        tweaked_path = Cow::Owned(trimmed.to_string());
    }

    // Handle file paths with spaces and other chars;
    // see https://github.com/coreos/rpm-ostree/issues/2029 */
    utils::shellsafe_quote(tweaked_path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_canonicalize_path() {
        let canonical = &["/", "/usr", "../usr/share", "../../usr/lib/systemd/system"];
        for &k in canonical {
            assert_eq!(k, canonicalize_path(k));
        }
        let noncanonical = &[
            ("./././foo", "./foo"),
            ("../../..//sbin/chkconfig", "../../../sbin/chkconfig"),
        ];
        for k in noncanonical {
            assert_eq!(canonicalize_path(k.0), k.1);
        }
    }

    #[test]
    fn test_path_is_compliant() {
        let ostree_cases = &["/", "/usr", "/usr/share", "/bin/foo", "/usr/lib/opt/bar"];
        for entry in ostree_cases {
            assert_eq!(path_is_ostree_compliant(entry), true, "{}", entry);
        }

        let opt_cases = &["/opt", "/opt/misc"];
        for entry in opt_cases {
            assert_eq!(path_is_ostree_compliant(entry), false, "{}", entry);
        }

        let denied_cases = &["/var", "/etc", "/var/run", "/usr/local", "", "./", "usr/"];
        for entry in denied_cases {
            assert_eq!(path_is_ostree_compliant(entry), false, "{}", entry);
        }
    }

    #[test]
    fn test_fix_tmpfiles_path() {
        let intact_cases = vec!["/", "/var", "/var/foo", "/run/foo"];
        for entry in intact_cases {
            let output = fix_tmpfiles_path(Cow::Borrowed(entry));
            assert_eq!(output, entry);
        }

        let quoting_cases = maplit::btreemap! {
            "/var/foo bar" => r#"'/var/foo bar'"#,
            "/var/run/" => "/run/",
            "/var/run/foo bar" => r#"'/run/foo bar'"#,
        };
        for (input, expected) in quoting_cases {
            let output = fix_tmpfiles_path(Cow::Borrowed(input));
            assert_eq!(output, expected);
        }
    }

    #[test]
    fn test_translate_to_tmpfiles_d() {
        let path = r#"/var/foo bar"#;
        let username = "testuser";
        let groupname = "testgroup";
        {
            // Directory
            let file_info = FileInfo::new();
            file_info.set_file_type(FileType::Directory);
            file_info.set_attribute_uint32("unix::mode", 0o721);
            let out = translate_to_tmpfiles_d(&path, &file_info, &username, &groupname).unwrap();
            let expected = r#"d '/var/foo bar' 0721 testuser testgroup - -"#;
            assert_eq!(out, expected);
        }
        {
            // Symlink
            let file_info = FileInfo::new();
            file_info.set_file_type(FileType::SymbolicLink);
            file_info.set_symlink_target("/mytarget");
            let out = translate_to_tmpfiles_d(&path, &file_info, &username, &groupname).unwrap();
            let expected = r#"L '/var/foo bar' - - - - /mytarget"#;
            assert_eq!(out, expected);
        }
        {
            // File
            let file_info = FileInfo::new();
            file_info.set_file_type(FileType::Regular);
            file_info.set_attribute_uint32("unix::mode", 0o123);
            let out = translate_to_tmpfiles_d(&path, &file_info, &username, &groupname).unwrap();
            let expected = r#"f '/var/foo bar' 0123 testuser testgroup - -"#;
            assert_eq!(out, expected);
        }
        {
            // Other unsupported
            let file_info = FileInfo::new();
            file_info.set_file_type(FileType::Unknown);
            translate_to_tmpfiles_d(&path, &file_info, &username, &groupname).unwrap_err();
        }
    }

    #[test]
    fn test_importer_tmpfiles_symlinks() {
        let mut importer = RpmImporter::new();

        {
            let normal_paths = [
                "usr/lib/foo",
                "var/lib/foo",
                "var/lib/alternatives/foo",
                "usr/opt/foo",
            ];
            for testcase in normal_paths {
                let matched = importer.inspect_path_for_symlink_translation(testcase);
                assert!(!matched);
            }
            let lines = importer.tmpfiles_symlink_entries();
            assert!(lines.is_empty());
        }
        {
            let special_paths = [
                "var/lib/vagrant",
                "var/lib/alternatives",
                "opt/foo",
                "opt/bar/first",
            ];
            for testcase in special_paths {
                let matched = importer.inspect_path_for_symlink_translation(testcase);
                assert!(matched);
            }
            let lines = importer.tmpfiles_symlink_entries();
            let expected = vec![
                "L /opt/bar - - - - ../../usr/lib/opt/bar".to_string(),
                "L /opt/foo - - - - ../../usr/lib/opt/foo".to_string(),
                "L /var/lib/alternatives - - - - ../../usr/lib/alternatives".to_string(),
                "L /var/lib/vagrant - - - - ../../usr/lib/vagrant".to_string(),
            ];
            assert_eq!(lines, expected);
        }
    }

    #[test]
    fn test_importer_get_first_path_element() {
        let cases = [("foo", "foo"), ("bar/", "bar"), ("xxx/yyy/zzz", "xxx")];

        for (input, expected) in cases {
            let output = RpmImporter::get_first_path_element(input);
            assert_eq!(output, expected, "Input: {input}");
        }
    }
}
