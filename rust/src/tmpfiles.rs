/*
 * Copyright (C) 2023 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
use crate::cxxrsutil::*;
use crate::ffiutil;

use anyhow::{Context, Result};
use cap_std::fs::{Dir, Permissions};
use cap_std_ext::dirext::CapStdExtDirExt;
use fn_error_context::context;
use std::collections::{HashMap, HashSet};
use std::fmt::Write;
use std::os::unix::prelude::PermissionsExt;
use std::path::Path;

const TMPFILESD: &str = "usr/lib/tmpfiles.d";
const RPMOSTREE_TMPFILESD: &str = "usr/lib/rpm-ostree/tmpfiles.d";
const AUTOVAR_PATH: &str = "rpm-ostree-autovar.conf";

#[context("Deduplicate tmpfiles entries")]
pub fn deduplicate_tmpfiles_entries(tmprootfs_dfd: i32) -> CxxResult<()> {
    let tmprootfs_dfd = unsafe { ffiutil::ffi_dirfd(tmprootfs_dfd)? };

    // scan all rpm-ostree auto generated entries and save
    let tmpfiles_dir = tmprootfs_dfd
        .open_dir(RPMOSTREE_TMPFILESD)
        .context("readdir {RPMOSTREE_TMPFILESD}")?;
    let mut rpmostree_tmpfiles_entries = save_tmpfile_entries(&tmpfiles_dir)?
        .map(|s| {
            let entry = tmpfiles_entry_get_path(&s.as_str())?;
            anyhow::Ok((entry.to_string(), s.to_string()))
        })
        .collect::<Result<HashMap<String, String>>>()?;

    // remove autovar.conf first, then scan all system entries and save
    let tmpfiles_dir = tmprootfs_dfd
        .open_dir(TMPFILESD)
        .context("readdir {TMPFILESD}")?;

    if tmpfiles_dir.try_exists(AUTOVAR_PATH)? {
        tmpfiles_dir.remove_file(AUTOVAR_PATH)?;
    }
    let system_tmpfiles_entries = save_tmpfile_entries(&tmpfiles_dir)?
        .map(|s| {
            let entry = tmpfiles_entry_get_path(&s.as_str())?;
            anyhow::Ok(entry.to_string())
        })
        .collect::<Result<HashSet<String>>>()?;

    // remove duplicated entries in auto-generated tmpfiles.d,
    // which are already in system tmpfiles
    for key in system_tmpfiles_entries.into_iter() {
        rpmostree_tmpfiles_entries.retain(|k, _value| k != &key);
    }

    {
        // save the noduplicated entries
        let mut entries = String::from("# This file was generated by rpm-ostree.\n");
        for (_key, value) in rpmostree_tmpfiles_entries {
            writeln!(entries, "{value}").unwrap();
        }

        let perms = Permissions::from_mode(0o644);
        tmpfiles_dir.atomic_write_with_perms(&AUTOVAR_PATH, entries.as_bytes(), perms)?;
    }
    Ok(())
}

// #[context("Scan all tmpfiles conf and save entries")]
fn save_tmpfile_entries(tmpfiles_dir: &Dir) -> Result<impl Iterator<Item = String>> {
    let entries = tmpfiles_dir
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
                    .unwrap()
                    .lines()
                    .filter(|s| !s.is_empty() && !s.starts_with('#'))
                    .map(|s| s.to_string())
                    .collect::<Vec<_>>(),
            )
        })
        .flatten()
        .collect::<Vec<_>>();

    Ok(entries.into_iter())
}

#[context("Scan tmpfiles entries and get path")]
fn tmpfiles_entry_get_path(line: &str) -> Result<&str> {
    line.split_whitespace()
        .nth(1)
        .ok_or_else(|| anyhow::anyhow!("Malformed tmpfiles.d entry ({line})"))
}

#[cfg(test)]
mod tests {
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
}
