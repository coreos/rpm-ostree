//! Functions for normalising various parts of the build.
//! The general goal is for the same input to generate the
//! same ostree commit hash each time.

// Copyright (C) 2021 Oracle and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::nameservice::shadow::parse_shadow_content;
use anyhow::Result;
use fn_error_context::context;
use lazy_static::lazy_static;
use std::io::{BufReader, Seek, SeekFrom};

lazy_static! {
    static ref SOURCE_DATE_EPOCH_RAW: Option<String> = std::env::var("SOURCE_DATE_EPOCH").ok();
    static ref SOURCE_DATE_EPOCH: Option<i64> = SOURCE_DATE_EPOCH_RAW
        .as_ref()
        .map(|s| s.parse::<i64>().expect("bad number in SOURCE_DATE_EPOCH"));
}

pub(crate) fn source_date_epoch_raw() -> Option<&'static str> {
    SOURCE_DATE_EPOCH_RAW.as_ref().map(|s| s.as_str())
}

#[context("Rewriting /etc/shadow to remove lstchg field")]
pub(crate) fn normalize_etc_shadow(rootfs: &openat::Dir) -> Result<()> {
    // Read in existing entries.
    let mut shadow = rootfs.update_file("usr/etc/shadow", 0o400)?;
    let entries = parse_shadow_content(BufReader::new(&mut shadow))?;

    // Go back to the start and truncate the file.
    shadow.seek(SeekFrom::Start(0))?;
    shadow.set_len(0)?;

    for mut entry in entries {
        // Entries starting with `!` or `*` indicate accounts that are
        // either locked or not using passwords. The last password
        // change value can be safely blanked for these.
        if entry.pwdp.starts_with('!') || entry.pwdp.starts_with('*') {
            entry.lstchg = None;
        }

        entry.to_writer(&mut shadow)?;
    }

    Ok(())
}
