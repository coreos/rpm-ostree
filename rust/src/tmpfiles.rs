/*
 * Copyright (C) 2023 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use std::borrow::Cow;
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write;
use std::io::Write as StdWrite;
use std::path::{Path, PathBuf};

use anyhow::bail;
use anyhow::{Context, Result};
use camino::Utf8Path;
use camino::Utf8PathBuf;
use cap_std::fs::{Dir, Permissions, PermissionsExt};
use cap_std::fs::{DirBuilderExt, MetadataExt};
use cap_std::io_lifetimes::AsFilelike;
use cap_std_ext::dirext::CapStdExtDirExt;
use fn_error_context::context;
use ostree_ext::gio;
use ostree_ext::gio::{FileInfo, FileType};
use ostree_ext::prelude::CancellableExtManual;

use crate::cxxrsutil::*;
use crate::ffiutil;

const TMPFILESD: &str = "usr/lib/tmpfiles.d";
/// A "staging" tmpfiles.d location generated when importing packages
/// in rpmostree-importer.cxx.
const RPMOSTREE_TMPFILESD: &str = "usr/lib/rpm-ostree/tmpfiles.d";
/// The final merged tmpfiles.d entry we generate from the per-package
/// "staged" versions stored above.
const AUTOVAR_PATH: &str = "rpm-ostree-autovar.conf";

fn fix_tmpfiles_path(abs_path: std::borrow::Cow<str>) -> Cow<str> {
    let mut tweaked_path = abs_path;

    // systemd-tmpfiles complains loudly about writing to /var/run;
    // ideally, all of the packages get fixed for this but...eh.
    if tweaked_path.starts_with("/var/run/") {
        let trimmed = tweaked_path.trim_start_matches("/var");
        tweaked_path = Cow::Owned(trimmed.to_string());
    }

    // Handle file paths with spaces and other chars;
    // see https://github.com/coreos/rpm-ostree/issues/2029 */
    crate::utils::shellsafe_quote(tweaked_path)
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
            .ok_or_else(|| anyhow::anyhow!("missing symlink target"))?;
        let link_target = link_target
            .to_str()
            .ok_or_else(|| anyhow::anyhow!("Invalid non-UTF8 symlink: {link_target:?}"))?;
        write!(&mut bufwr, " - - - - {}", link_target)?;
    } else {
        let mode = file_info.attribute_uint32("unix::mode") & !libc::S_IFMT;
        write!(&mut bufwr, " {:04o} {} {} - -", mode, username, groupname)?;
    };

    Ok(bufwr)
}

pub fn tmpfiles_translate(
    abs_path: &str,
    file_info: &crate::FFIGFileInfo,
    username: &str,
    groupname: &str,
) -> CxxResult<String> {
    let entry = translate_to_tmpfiles_d(abs_path, &file_info.glib_reborrow(), username, groupname)?;
    Ok(entry)
}

#[context("Converting /var to tmpfiles.d")]
pub(crate) fn var_to_tmpfiles(rootfs: &Dir, cancellable: Option<&gio::Cancellable>) -> Result<()> {
    /* List of files that are known to possibly exist, but in practice
     * things work fine if we simply ignore them.  Don't add something
     * to this list unless you've verified it's handled correctly at
     * runtime.  (And really both in CentOS and Fedora)
     */
    static KNOWN_STATE_FILES: &[&str] = &[
        // https://bugzilla.redhat.com/show_bug.cgi?id=789407
        "var/lib/systemd/random-seed",
        "var/lib/systemd/catalog/database",
        "var/lib/plymouth/boot-duration",
        // These two are part of systemd's var.tmp
        "var/log/wtmp",
        "var/log/btmp",
    ];

    let pwdb = crate::PasswdDB::populate_new(rootfs)?;

    // We never want to traverse into /run when making tmpfiles since it's a tmpfs
    // Note that in a Fedora root, /var/run is a symlink, though on el7, it can be a dir.
    // See: https://github.com/projectatomic/rpm-ostree/pull/831
    if rootfs.try_exists("var/run")? {
        rootfs
            .remove_dir_all("var/run")
            .context("Failed to remove /var/run")?;
    }

    // Here, delete some files ahead of time to avoid emitting warnings
    // for things that are known to be harmless.
    for path in KNOWN_STATE_FILES {
        rootfs
            .remove_file_optional(*path)
            .with_context(|| format!("unlinkat({})", path))?;
    }

    // Convert /var wholesale to tmpfiles.d. Note that with unified core, this
    // code should no longer be necessary as we convert packages on import.
    // Make output file world-readable, no reason why not to
    // https://bugzilla.redhat.com/show_bug.cgi?id=1631794
    let mut db = cap_std::fs::DirBuilder::new();
    db.recursive(true);
    db.mode(0o755);
    rootfs.create_dir_with("usr/lib/tmpfiles.d", &db)?;
    let mode = Permissions::from_mode(0o644);
    rootfs.atomic_replace_with(
        "usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf",
        |bufwr| -> Result<()> {
            bufwr.get_mut().as_file_mut().set_permissions(mode)?;
            let mut prefix = Utf8PathBuf::from("var");
            let mut entries = BTreeSet::new();
            convert_path_to_tmpfiles_d_recurse(
                &mut entries,
                &pwdb,
                rootfs,
                &mut prefix,
                &cancellable,
            )
            .with_context(|| format!("Processing var content /{}", prefix))?;
            for line in entries {
                bufwr.write_all(line.as_bytes())?;
                writeln!(bufwr)?;
            }
            Ok(())
        },
    )?;

    Ok(())
}

/// Recursively explore target directory and translate content to tmpfiles.d entries. See
/// `convert_var_to_tmpfiles_d` for more background.
///
/// This proceeds depth-first and progressively deletes translated subpaths as it goes.
/// `prefix` is updated at each recursive step, so that in case of errors it can be
/// used to pinpoint the faulty path.
#[allow(clippy::nonminimal_bool)]
fn convert_path_to_tmpfiles_d_recurse(
    out_entries: &mut BTreeSet<String>,
    pwdb: &crate::PasswdDB,
    rootfs: &Dir,
    prefix: &mut Utf8PathBuf,
    cancellable: &Option<&gio::Cancellable>,
) -> Result<()> {
    let current_prefix = prefix.clone();
    for subpath in rootfs.read_dir(&current_prefix).context("Reading dir")? {
        if let Some(c) = cancellable {
            c.set_error_if_cancelled()?;
        }

        let subpath = subpath?;
        let meta = subpath.metadata()?;
        let fname: Utf8PathBuf = PathBuf::from(subpath.file_name()).try_into()?;
        let full_path = Utf8Path::new(&current_prefix).join(&fname);

        // Workaround for nfs-utils in RHEL7:
        // https://bugzilla.redhat.com/show_bug.cgi?id=1427537
        let retain_entry = meta.is_file() && full_path.starts_with("var/lib/nfs");
        if !retain_entry && !(meta.is_dir() || meta.is_symlink()) {
            rootfs
                .remove_file_optional(&full_path)
                .with_context(|| format!("Removing {:?}", &full_path))?;
            println!("Ignoring non-directory/non-symlink '{:?}'", &full_path);
            continue;
        }

        // Translate this file entry.
        let entry = {
            let mode = meta.mode() & !libc::S_IFMT;

            let file_info = gio::FileInfo::new();
            file_info.set_attribute_uint32("unix::mode", mode);

            if meta.is_dir() {
                file_info.set_file_type(FileType::Directory);
            } else if meta.is_symlink() {
                file_info.set_file_type(FileType::SymbolicLink);
                let link_target = cap_primitives::fs::read_link_contents(
                    &rootfs.as_filelike_view(),
                    full_path.as_std_path(),
                )
                .context("Reading symlink")?;
                let link_target = link_target.to_str().ok_or_else(|| {
                    anyhow::anyhow!("non UTF-8 symlink target '{:?}'", link_target)
                })?;
                file_info.set_symlink_target(link_target);
            } else if meta.is_file() {
                file_info.set_file_type(FileType::Regular);
            } else {
                unreachable!("invalid path type: {:?}", full_path);
            }

            let abs_path = Utf8Path::new("/").join(&full_path);
            let username = pwdb.lookup_user(meta.uid())?;
            let groupname = pwdb.lookup_group(meta.gid())?;
            translate_to_tmpfiles_d(abs_path.as_str(), &file_info, &username, &groupname)?
        };
        out_entries.insert(entry);

        if meta.is_dir() {
            // New subdirectory discovered, recurse into it.
            *prefix = full_path.clone();
            convert_path_to_tmpfiles_d_recurse(out_entries, pwdb, rootfs, prefix, cancellable)?;
            rootfs.remove_dir_all(&full_path)?;
        } else {
            rootfs.remove_file(&full_path)?;
        }
    }
    Ok(())
}

#[context("Deduplicate tmpfiles entries")]
pub fn deduplicate_tmpfiles_entries(tmprootfs_dfd: i32) -> CxxResult<()> {
    let tmprootfs_dfd = unsafe { ffiutil::ffi_dirfd(tmprootfs_dfd)? };

    // scan all rpm-ostree auto generated entries and save
    let tmpfiles_dir = tmprootfs_dfd
        .open_dir_optional(RPMOSTREE_TMPFILESD)
        .context(RPMOSTREE_TMPFILESD)?;
    let mut rpmostree_tmpfiles_entries = if let Some(tmpfiles_dir) = tmpfiles_dir {
        read_tmpfiles(&tmpfiles_dir)?
    } else {
        Default::default()
    };

    // remove autovar.conf first, then scan all system entries and save
    let tmpfiles_dir = if let Some(d) = tmprootfs_dfd
        .open_dir_optional(TMPFILESD)
        .context(TMPFILESD)?
    {
        d
    } else {
        if !rpmostree_tmpfiles_entries.is_empty() {
            return Err(
                format!("No {TMPFILESD} directory found, but have tmpfiles to process").into(),
            );
        }
        // Nothing to do here
        return Ok(());
    };

    if tmpfiles_dir.try_exists(AUTOVAR_PATH)? {
        tmpfiles_dir.remove_file(AUTOVAR_PATH)?;
    }
    let system_tmpfiles_entries = read_tmpfiles(&tmpfiles_dir)?;

    // remove duplicated entries in auto-generated tmpfiles.d,
    // which are already in system tmpfiles
    for (key, _) in system_tmpfiles_entries {
        rpmostree_tmpfiles_entries.retain(|k, _value| k != &key);
    }

    {
        // save the noduplicated entries
        let mut entries = String::from("# This file was generated by rpm-ostree.\n");
        for (_key, value) in rpmostree_tmpfiles_entries {
            writeln!(entries, "{value}").unwrap();
        }

        let perms = Permissions::from_mode(0o644);
        tmpfiles_dir.atomic_write_with_perms(AUTOVAR_PATH, entries.as_bytes(), perms)?;
    }
    Ok(())
}

/// Read all tmpfiles.d entries in the target directory, and return a mapping
/// from (file path) => (single tmpfiles.d entry line)
#[context("Read systemd tmpfiles.d")]
fn read_tmpfiles(tmpfiles_dir: &Dir) -> Result<BTreeMap<String, String>> {
    tmpfiles_dir
        .entries()?
        .filter_map(|name| {
            let name = name.unwrap().file_name();
            if let Some(extension) = Path::new(&name).extension() {
                if extension != "conf" {
                    return None;
                }
            } else {
                return None;
            }
            Some(
                tmpfiles_dir
                    .read_to_string(name)
                    .ok()?
                    .lines()
                    .filter(|s| !s.is_empty() && !s.starts_with('#'))
                    .map(|s| s.to_string())
                    .collect::<Vec<_>>(),
            )
        })
        .flatten()
        .map(|s| {
            let entry = tmpfiles_entry_get_path(s.as_str())?;
            anyhow::Ok((entry.to_string(), s))
        })
        .collect()
}

#[context("Scan tmpfiles entries and get path")]
fn tmpfiles_entry_get_path(line: &str) -> Result<&str> {
    line.split_whitespace()
        .nth(1)
        .ok_or_else(|| anyhow::anyhow!("Malformed tmpfiles.d entry ({line})"))
}

#[cfg(test)]
mod tests {
    use cap_std::fs::DirBuilder;
    use cap_std_ext::cap_tempfile;
    use rustix::fd::AsRawFd;

    use super::*;
    #[test]
    fn test_tmpfiles_entry_get_path() {
        let cases = [
            ("z /dev/kvm          0666 - kvm -", "/dev/kvm"),
            ("d /run/lock/lvm 0700 root root -", "/run/lock/lvm"),
            ("a+      /var/lib/tpm2-tss/system/keystore   -    -    -     -           default:group:tss:rwx", "/var/lib/tpm2-tss/system/keystore"),
        ];
        for (input, expected) in cases {
            let path = tmpfiles_entry_get_path(input).unwrap();
            assert_eq!(path, expected, "Input: {input}");
        }
    }

    fn newroot() -> Result<cap_std_ext::cap_tempfile::TempDir> {
        let root = cap_std_ext::cap_tempfile::tempdir(cap_std::ambient_authority())?;
        root.create_dir_all(RPMOSTREE_TMPFILESD)?;
        root.create_dir_all(TMPFILESD)?;
        Ok(root)
    }

    #[test]
    fn test_deduplicate_noop() -> Result<()> {
        let root = &newroot()?;
        deduplicate_tmpfiles_entries(root.as_raw_fd())?;
        Ok(())
    }

    // The first and 3rd are duplicates
    const PKG_FILESYSTEM_CONTENTS: &str = indoc::indoc! { r#"
d /var/cache 0755 root root - -
d /var/lib/games 0755 root root - -
d /var/tmp 1777 root root - -
d /var/spool/mail 0775 root mail - -
"# };
    const VAR_CONF: &str = indoc::indoc! { r#"
d /var/cache 0755 - - -
"# };
    const TMP_CONF: &str = indoc::indoc! { r#"
q /var/tmp 1777 root root 30d
"# };

    #[test]
    fn test_deduplicate() -> Result<()> {
        let root = &newroot()?;
        let rpmostree_tmpfiles_dir = root.open_dir(RPMOSTREE_TMPFILESD)?;
        let tmpfiles_dir = root.open_dir(TMPFILESD)?;
        rpmostree_tmpfiles_dir
            .atomic_write(format!("pkg-filesystem.conf"), PKG_FILESYSTEM_CONTENTS)?;
        tmpfiles_dir.atomic_write("var.conf", VAR_CONF)?;
        tmpfiles_dir.atomic_write("tmp.conf", TMP_CONF)?;
        assert!(!tmpfiles_dir.try_exists(AUTOVAR_PATH)?);
        deduplicate_tmpfiles_entries(root.as_raw_fd())?;
        let contents = tmpfiles_dir.read_to_string(AUTOVAR_PATH).unwrap();
        assert!(contents.contains("# This file was generated by rpm-ostree."));
        let entries = contents
            .lines()
            .filter(|l| !(l.is_empty() || l.starts_with('#')))
            .collect::<Vec<_>>();
        assert_eq!(entries.len(), 2);
        assert_eq!(entries[0], "d /var/lib/games 0755 root root - -");
        assert_eq!(entries[1], "d /var/spool/mail 0775 root mail - -");
        Ok(())
    }

    #[test]
    /// Verify that we no-op if the directories don't exist.
    fn test_deduplicate_emptydir() -> Result<()> {
        let root = &cap_std_ext::cap_tempfile::tempdir(cap_std::ambient_authority())?;
        deduplicate_tmpfiles_entries(root.as_raw_fd())?;
        Ok(())
    }

    #[test]
    fn test_tmpfiles_d_translation() {
        use nix::sys::stat::{umask, Mode};
        use rustix::process::{getegid, geteuid};

        // Create an empty file with the given mode
        fn touch(d: &Dir, p: impl AsRef<Utf8Path>, mode: u32) -> Result<()> {
            d.atomic_replace_with(p.as_ref(), |w| {
                w.get_mut()
                    .as_file_mut()
                    .set_permissions(Permissions::from_mode(mode))
                    .map_err(Into::into)
            })
        }

        let mut db = DirBuilder::new();
        db.recursive(true);
        db.mode(0o755);

        // Prepare a minimal rootfs as playground.
        umask(Mode::empty());
        let rootfs = cap_tempfile::tempdir(cap_std::ambient_authority()).unwrap();
        let uid = geteuid().as_raw();
        let gid = getegid().as_raw();
        let uid_str = format!("{uid}");
        let gid_str = format!("{gid}");
        let mut expected_disk_size = 30u64;
        {
            for dirpath in &["usr/lib", "usr/etc", "var"] {
                rootfs.ensure_dir_with(*dirpath, &db).unwrap();
            }
            for filepath in &["usr/lib/passwd", "usr/lib/group"] {
                touch(&rootfs, *filepath, 0o755).unwrap()
            }
            rootfs
                .atomic_write(
                    "usr/etc/passwd",
                    format!("test-user:x:{uid_str}:{gid_str}:::",),
                )
                .unwrap();
            expected_disk_size += uid_str.len() as u64;
            expected_disk_size += gid_str.len() as u64;
            rootfs
                .atomic_write("usr/etc/group", format!("test-group:x:{gid_str}:"))
                .unwrap();
            expected_disk_size += gid_str.len() as u64;
        }

        // Add test content.
        rootfs.ensure_dir_with("var/lib/systemd", &db).unwrap();
        touch(&rootfs, "var/lib/systemd/random-seed", 0o770).unwrap();
        rootfs.ensure_dir_with("var/lib/nfs", &db).unwrap();
        touch(&rootfs, "var/lib/nfs/etab", 0o770).unwrap();
        db.mode(0o777);
        rootfs.ensure_dir_with("var/lib/test/nested", &db).unwrap();
        touch(&rootfs, "var/lib/test/nested/file", 0o770).unwrap();
        rootfs
            .symlink("../", "var/lib/test/nested/symlink")
            .unwrap();
        cap_primitives::fs::symlink_contents(
            "/var/lib/foo",
            &rootfs.as_filelike_view(),
            "var/lib/test/absolute-symlink",
        )
        .unwrap();

        // Also make this a sanity test for our directory size API
        let cancellable = gio::Cancellable::new();
        assert_eq!(
            crate::directory_size(rootfs.as_raw_fd(), cancellable.reborrow_cxx()).unwrap(),
            expected_disk_size
        );

        crate::var_to_tmpfiles(&rootfs, gio::Cancellable::NONE).unwrap();

        let autovar_path = "usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf";
        assert!(!rootfs.try_exists("var/lib").unwrap());
        assert!(rootfs.try_exists(autovar_path).unwrap());
        let entries: Vec<String> = rootfs
            .read_to_string(autovar_path)
            .unwrap()
            .lines()
            .map(|s| s.to_owned())
            .collect();
        let expected = &[
            "L /var/lib/test/absolute-symlink - - - - /var/lib/foo",
            "L /var/lib/test/nested/symlink - - - - ../",
            "d /var/lib 0755 test-user test-group - -",
            "d /var/lib/nfs 0755 test-user test-group - -",
            "d /var/lib/systemd 0755 test-user test-group - -",
            "d /var/lib/test 0777 test-user test-group - -",
            "d /var/lib/test/nested 0777 test-user test-group - -",
            "f /var/lib/nfs/etab 0770 test-user test-group - -",
        ];
        assert_eq!(entries, expected, "{:#?}", entries);
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
            let out = translate_to_tmpfiles_d(path, &file_info, username, groupname).unwrap();
            let expected = r#"d '/var/foo bar' 0721 testuser testgroup - -"#;
            assert_eq!(out, expected);
        }
        {
            // Symlink
            let file_info = FileInfo::new();
            file_info.set_file_type(FileType::SymbolicLink);
            file_info.set_symlink_target("/mytarget");
            let out = translate_to_tmpfiles_d(path, &file_info, username, groupname).unwrap();
            let expected = r#"L '/var/foo bar' - - - - /mytarget"#;
            assert_eq!(out, expected);
        }
        {
            // File
            let file_info = FileInfo::new();
            file_info.set_file_type(FileType::Regular);
            file_info.set_attribute_uint32("unix::mode", 0o123);
            let out = translate_to_tmpfiles_d(path, &file_info, username, groupname).unwrap();
            let expected = r#"f '/var/foo bar' 0123 testuser testgroup - -"#;
            assert_eq!(out, expected);
        }
        {
            // Other unsupported
            let file_info = FileInfo::new();
            file_info.set_file_type(FileType::Unknown);
            translate_to_tmpfiles_d(path, &file_info, username, groupname).unwrap_err();
        }
    }
}
