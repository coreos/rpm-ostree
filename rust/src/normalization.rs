//! Functions for normalising various parts of the build.
//! The general goal is for the same input to generate the
//! same ostree commit hash each time.

// Copyright (C) 2021 Oracle and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::nameservice::shadow::parse_shadow_content;
use anyhow::{anyhow, Result};
use fn_error_context::context;
use std::convert::TryInto;
use std::io::{BufReader, Read, Seek, SeekFrom, Write};

pub(crate) fn source_date_epoch() -> Option<i64> {
    if let Some(raw) = source_date_epoch_raw() {
        raw.parse().ok()
    } else {
        None
    }
}

pub(crate) fn source_date_epoch_raw() -> Option<String> {
    std::env::var("SOURCE_DATE_EPOCH").ok()
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

const RPM_HEADER_MAGIC: [u8; 8] = [0x8E, 0xAD, 0xE8, 0x01, 0x00, 0x00, 0x00, 0x00];
const RPMTAG_INSTALLTIME: u32 = 1008;
const RPMTAG_INSTALLTID: u32 = 1128;

#[context("Normalizing rpmdb timestamps for build stability")]
pub(crate) fn rewrite_rpmdb_timestamps<F: Read + Write + Seek>(rpmdb: &mut F) -> Result<()> {
    let source_date = if let Some(source_date) = source_date_epoch() {
        source_date as u32
    } else {
        return Ok(());
    };

    // Remember where we started
    let pos = rpmdb.stream_position()?;

    let mut buffer: [u8; 16] = [0; 16];
    let install_tid = source_date;
    let mut install_time = source_date;

    loop {
        // Read in a header record
        match rpmdb.read_exact(&mut buffer) {
            Err(ref e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e.into()),
            _ => (),
        };

        // Make sure things are sane
        if buffer[..8] != RPM_HEADER_MAGIC {
            return Err(anyhow!("Bad RPM header magic in RPM database"));
        }

        // Grab the count of index records and the size of the data blob
        let record_count = u32::from_be_bytes(buffer[8..12].try_into()?);
        let data_size = u32::from_be_bytes(buffer[12..].try_into()?);

        // Loop through the records looking for ones that point at things
        // that are, or are derived from, timestamps
        let mut offsets = Vec::new();
        for _ in 0..record_count {
            rpmdb.read_exact(&mut buffer)?;

            let tag = u32::from_be_bytes(buffer[..4].try_into()?);
            if tag == RPMTAG_INSTALLTIME || tag == RPMTAG_INSTALLTID {
                offsets.push((tag, u32::from_be_bytes(buffer[8..12].try_into()?)));
            }
        }

        // Work through the data blob replacing the timestamp-derived values
        // with the timestamp we want
        offsets.sort_unstable_by_key(|(_, offset)| *offset);
        let mut offset = 0;
        for (tag, value_offset) in offsets {
            rpmdb.seek(std::io::SeekFrom::Current((value_offset - offset) as i64))?;
            if tag == RPMTAG_INSTALLTID {
                rpmdb.write_all(&install_tid.to_be_bytes())?;
            } else if tag == RPMTAG_INSTALLTIME {
                rpmdb.write_all(&install_time.to_be_bytes())?;
                install_time += 1;
            }
            offset = value_offset + std::mem::size_of::<u32>() as u32;
        }

        // Move to the next record
        rpmdb.seek(std::io::SeekFrom::Current((data_size - offset) as i64))?;
    }

    // Seek back to where we were before
    rpmdb.seek(std::io::SeekFrom::Start(pos))?;

    Ok(())
}
