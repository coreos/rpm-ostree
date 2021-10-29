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

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::Result;
    use openssl::sha::sha256;
    use std::io::Cursor;

    #[test]
    fn rpmdb_timestamp_rewrite() -> Result<()> {
        // This is a pretty simple smoke test. We have a dummy RPM database
        // dump that contains a specific initial timestamp in several tags:
        //
        //  - RPMTAG_INSTALLTID
        //  - RPMTAG_INSTALLTIME
        //  - RPMTAG_BUILDTIME
        //
        // The first two are the ones we want changed, the latter is a canary
        // to ensure we don't get overzealous.
        //
        // We know the checksums of both the unrewritten and rewritten dumps
        // so all we need to do is make sure that the initial database matches
        // what we expect, and then run the rewrite, and ensure the new
        // checksum matches what we expect.
        //
        // More complicated testing is left to other test cycles.

        let rpmdb = include_bytes!("../test/dummy-rpm-database.bin").to_vec();

        const REWRITE_TIMESTAMP: u32 = 1445437680;
        const INITIAL_CHECKSUM: [u8; 32] = [
            0x66, 0xac, 0x68, 0x75, 0xe7, 0x40, 0x99, 0x64, 0xd0, 0x04, 0xde, 0xff, 0x09, 0x80,
            0x22, 0x77, 0xb0, 0xeb, 0x63, 0x7a, 0xa9, 0x14, 0x62, 0x4e, 0xda, 0x52, 0x36, 0x06,
            0x8b, 0x23, 0x39, 0xec,
        ];
        const REWRITE_CHECKSUM: [u8; 32] = [
            0xac, 0x79, 0xb9, 0xa9, 0x9b, 0x95, 0x73, 0x81, 0x5f, 0x7c, 0x90, 0xbb, 0x27, 0x49,
            0x55, 0xba, 0x1a, 0x77, 0xcd, 0xfc, 0xde, 0x6e, 0xa0, 0xf9, 0xc4, 0x9c, 0x6e, 0xea,
            0x88, 0x31, 0x15, 0x43,
        ];

        // Calculate and check initial checksum.
        let checksum = sha256(&rpmdb);
        assert_eq!(checksum[..], INITIAL_CHECKSUM[..]);

        // Override SOURCE_DATE_EPOCH, retaining original value for later.
        let source_date = std::env::var_os("SOURCE_DATE_EPOCH");
        std::env::set_var("SOURCE_DATE_EPOCH", REWRITE_TIMESTAMP.to_string());

        // Actually do the rewrite.
        let mut cursor = Cursor::new(rpmdb);
        rewrite_rpmdb_timestamps(&mut cursor)?;
        let rpmdb = cursor.into_inner();

        // Restore or remove the original SOURCE_DATE_EPOCH.
        if let Some(value) = source_date {
            std::env::set_var("SOURCE_DATE_EPOCH", value);
        } else {
            std::env::remove_var("SOURCE_DATE_EPOCH");
        }

        // Calculate and check checksum of rewritten data.
        let checksum = sha256(&rpmdb);
        assert_eq!(checksum[..], REWRITE_CHECKSUM[..]);

        Ok(())
    }
}
