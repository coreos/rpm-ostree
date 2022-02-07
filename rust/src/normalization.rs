//! Functions for normalising various parts of the build.
//! The general goal is for the same input to generate the
//! same ostree commit hash each time.

// Copyright (C) 2021 Oracle and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::bwrap::Bubblewrap;
use crate::nameservice::shadow::parse_shadow_content;
use anyhow::{anyhow, Result};
use fn_error_context::context;
use ostree_ext::gio;
use std::convert::TryInto;
use std::io::{BufReader, Read, Seek, SeekFrom, Write};
use std::path::Path;

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

#[context("Rewriting rpmdb database files for build stability")]
pub(crate) fn normalize_rpmdb(rootfs: &openat::Dir, rpmdb_path: impl AsRef<Path>) -> Result<()> {
    let source_date = if let Some(source_date) = source_date_epoch() {
        source_date as u32
    } else {
        return Ok(());
    };

    let mut bwrap =
        Bubblewrap::new_with_mutability(rootfs, crate::ffi::BubblewrapMutability::Immutable)?;
    bwrap.append_child_argv(&["rpm", "--eval", "%{_db_backend}"]);
    let cancellable = gio::Cancellable::new();
    let db_backend = bwrap.run_captured(Some(&cancellable))?;

    let db_backend = String::from_utf8(db_backend.to_vec())?;

    match db_backend.trim() {
        "bdb" => bdb_normalize::normalize(rootfs, rpmdb_path, source_date),
        "ndb" => Ok(()),
        "sqlite" => Ok(()),
        _ => Err(anyhow!("Unknown rpmdb backend: {}", db_backend)),
    }
}

mod bdb_normalize {
    // Gather round, my friends, and I shall tell you a tale of trade-offs and consequences.
    //
    // Way, way back in the halcyon days of 1994 a piece of database software came into being.
    // Computers were simpler then, and slower, and to save memory our database-creating
    // protagonists elected to use a page cache in which to keep pages of data from the
    // database. Then they elected to re-use pages from that cache that were no longer in use
    // when they needed to add new pages to a database. Then, to save time presumably, they
    // decided to not bother zeroing these pages because the left-over data was unimportant.
    //
    // This, as you can imagine, wreaks merry hell on any attempt at creating deterministic
    // output. But that's not all.
    //
    // When this software was extended to allow multiple access they needed a way to
    // identify a given database file for locking purposes. So they gave each file a unique
    // file ID. This also wreaks merry hell on any attempt at deterministic output, especially
    // since it doesn't respect any of the things that might help that such as SOURCE_DATE_EPOCH.
    //
    // This leads to the eternal question: What do?
    //
    // This code knows just enough about the structure of BerkeleyDB BTree and Hash database
    // files to know which bits are unused and writes zeros over those bits with extreme
    // prejudice. It also constructs a file ID based purely on a provided timestamp and the
    // name of the file in question. Both of these normalise the file sufficiently that we
    // no longer see byte-wise variance given the same input data.

    use crate::bwrap::Bubblewrap;
    use anyhow::{anyhow, Context, Result};
    use binread::{BinRead, BinReaderExt};
    use once_cell::sync::Lazy;
    use openat::SimpleType;
    use openssl::sha::{sha256, Sha256};
    use ostree_ext::gio;
    use std::io::{Read, Seek, SeekFrom, Write};
    use std::path::{Path, PathBuf};

    // BerkeleyDB page types, limited to those found in the BTree and Hash database types.
    #[derive(BinRead, Debug, Clone, Copy, PartialEq, Eq)]
    #[repr(u8)]
    #[br(repr=u8)]
    enum PageType {
        IBTree = 3,    // An internal BTree page
        LBTree = 5,    // A leaf BTree page
        Overflow = 7,  // An overflow page (for values too long to fit in a "normal" page)
        HashMeta = 8,  // A Hash metadata page
        BTreeMeta = 9, // A BTree metadata page
        Hash = 13,     // A Hash data page
    }

    // Database metadata header.
    #[derive(BinRead, Debug)]
    #[br(little)]
    #[allow(dead_code)]
    struct MetaHeader {
        lsn: u64,            // Log sequence number
        pgno: u32,           // Number of this page
        magic: u32,          // Magic (determines which type of database this is)
        version: u32,        // Database library version
        pagesize: u32,       // Page size for this database
        encrypt_alg: u8,     // Encryption algorithm (if used)
        page_type: PageType, // Type of this page
        metaflags: u8,       // Metadata flags
        unused1: u8,         // Exactly what it says
        free: u32,           // Free list page number
        last_pgno: u32,      // Number of last page in database
        nparts: u32,         // Numer of partitions
        key_count: u32,      // Cached key count
        record_count: u32,   // Cached record count
        flags: u32,          // Flags (type-dependent)
        uid: [u8; 20],       // File ID
    }

    // Database metadata magic number value for BTree database type.
    const BTREE_MAGIC: u32 = 0x00053162;

    // Database metadata magic number value for Hash database type.
    const HASH_MAGIC: u32 = 0x00061561;

    // Size of the page header structure
    const PAGE_HEADER_SIZE: u16 = 26;

    // Offset of the file ID field in the metadata header
    const PAGE_HEADER_FILE_ID_OFFSET: u64 = 0x34;

    // The per-header page used in both BTree and Hash databases.
    #[derive(BinRead, Debug)]
    #[br(little)]
    #[allow(dead_code)]
    struct PageHeader {
        lsn: u64,            // Log sequence number
        pgno: u32,           // Number of this page
        prev_pgno: u32,      // Number of the previous page
        next_pgno: u32,      // Number of the next page
        entries: u16,        // Number of entries in this page
        hf_offset: u16,      // Offset to the first free byte in this page
        level: u8,           // BTree depth (leaf is 1, grows upwards)
        page_type: PageType, // Type of this page
    }

    // The types of BTree items found in a page
    #[derive(BinRead, Debug)]
    #[br(repr=u8)]
    #[repr(u8)]
    enum BTreeItemType {
        KeyData = 1,   // Actual key/data values
        Duplicate = 2, // Duplicate entry
        Overflow = 3,  // Overflow
    }

    // A BTree item, defined as a length and a type. Data is stored later
    // in the page.
    #[derive(BinRead, Debug)]
    #[br(little)]
    struct BTreeItem {
        len: u16,                 // Length of this item
        item_type: BTreeItemType, // Type of this item
    }

    // The types of Hash items found in a page
    #[derive(BinRead, Debug)]
    #[br(repr=u8)]
    #[repr(u8)]
    enum HashItemType {
        KeyData = 1,   // Actual key/data values
        Duplicate = 2, // Duplicate entry
        Offpage = 3,   // Off-page (aka Overflow)
        OffDup = 4,    // Off-page duplicate
    }

    static PROC_SELF_CWD: Lazy<PathBuf> = Lazy::new(|| PathBuf::from("/proc/self/cwd"));

    pub(super) fn normalize_one(
        rootfs: &openat::Dir,
        path: &Path,
        entry: openat::Entry,
        timestamp: u32,
    ) -> Result<()> {
        // Construct a new, deterministic file ID.
        let mut file_id = Sha256::new();
        file_id.update(&timestamp.to_be_bytes());
        file_id.update(format!("bdb/{}", entry.file_name().to_str().unwrap()).as_bytes());
        let file_id = &file_id.finish()[..20];

        // Open the file for update.
        let mut db = rootfs.update_file(path, 0o644)?;

        // Get the metadata header and make sure we're working on one of the
        // types of file we care about.
        let meta_header: MetaHeader = db.read_le()?;
        match (meta_header.magic, meta_header.page_type) {
            (BTREE_MAGIC, PageType::BTreeMeta) => (),
            (HASH_MAGIC, PageType::HashMeta) => (),
            _ => return Ok(()),
        };

        // Seek to where the file ID lives and replace it.
        db.seek(SeekFrom::Start(PAGE_HEADER_FILE_ID_OFFSET))?;
        db.write_all(file_id)?;

        for pageno in 1..meta_header.last_pgno + 1 {
            // Seek to the next page.
            db.seek(SeekFrom::Start((pageno * meta_header.pagesize) as u64))?;

            // Read in the page header.
            let header: PageHeader = db.read_le()?;

            // If this is an overflow page then all we need to do is seek to the start
            // of free space and zero out the rest of the page.
            if header.page_type == PageType::Overflow {
                db.seek(SeekFrom::Current(header.hf_offset as i64))?;
                let fill_length = meta_header
                    .pagesize
                    .saturating_sub((PAGE_HEADER_SIZE + header.hf_offset) as u32);
                write_zeros(&mut db, fill_length)?;
                continue;
            }

            // For the other page types we have a series of 16-bit item offsets immediately
            // after the page header. We need to collect those up.
            let mut offsets: Vec<u16> = Vec::new();
            for _ in 0..header.entries {
                offsets.push(db.read_le()?);
            }
            offsets.sort_unstable();

            // Zero out the unused space after the item offsets. This will either be the
            // entire rest of the page if there aren't any or the space from the end of
            // the offset list to the start of the first item.
            let empty = if offsets.is_empty() {
                meta_header.pagesize - PAGE_HEADER_SIZE as u32
            } else {
                *offsets.first().unwrap() as u32 - (PAGE_HEADER_SIZE + header.entries * 2) as u32
            };
            write_zeros(&mut db, empty)?;

            let mut offset_iter = offsets.into_iter().peekable();
            while let Some(offset) = offset_iter.next() {
                // Seek to the next item offset.
                db.seek(SeekFrom::Start(
                    (pageno * meta_header.pagesize + offset as u32) as u64,
                ))?;

                if matches!(header.page_type, PageType::IBTree | PageType::LBTree) {
                    // BTree items consist of at least a 16-bit length and an 8-bit type.
                    let item: BTreeItem = db.read_le()?;
                    if header.page_type == PageType::IBTree {
                        // If this is an internal page (`IBTree`) then the byte immediately
                        // following the type field is unused. Zero it.
                        db.write_all(b"\x00")?;
                    } else if header.page_type == PageType::LBTree {
                        if let BTreeItemType::Overflow = item.item_type {
                            // BTree overflow entries don't use their length fields. Zero it.
                            db.seek(SeekFrom::Current(-3))?;
                            db.write_all(b"\x00\x00")?;
                        } else if let BTreeItemType::KeyData = item.item_type {
                            // Work out where the next item starts or if we're at the end of
                            // the page.
                            let next_offset = if let Some(next) = offset_iter.peek() {
                                *next
                            } else {
                                meta_header.pagesize as u16
                            };

                            // Zero out the space between the end of this item and the start
                            // of the next (or the end of the page).
                            let remainder = next_offset - (offset + 3 + item.len);
                            if remainder != 0 {
                                db.seek(SeekFrom::Current(item.len as i64))?;
                                write_zeros(&mut db, remainder)?;
                            }
                        }
                    }
                } else if header.page_type == PageType::Hash {
                    // Offpage (aka overflow) Hash entries have three unused bytes immediately
                    // after the 8-bit item type field. Zero them.
                    let item_type: HashItemType = db.read_le()?;

                    if let HashItemType::Offpage = item_type {
                        db.write_all(b"\x00\x00\x00")?;
                    }
                }
            }
        }

        db.flush()?;

        Ok(())
    }

    pub(super) fn normalize(
        rootfs: &openat::Dir,
        db_path: impl AsRef<Path>,
        timestamp: u32,
    ) -> Result<()> {
        let db_path = db_path.as_ref();

        for entry in rootfs.list_dir(db_path)? {
            let entry = entry?;

            // We only care about regular files.
            if !matches!(entry.simple_type(), Some(SimpleType::File)) {
                continue;
            }

            // We don't want any dotfiles, nor do we want to mess with the temporary
            // files BerkeleyDB sometimes leaves around.
            if entry
                .file_name()
                .to_str()
                .filter(|name| !(name.starts_with('.') || name.starts_with("__db")))
                .is_none()
            {
                continue;
            }

            let path = db_path.join(entry.file_name());

            // As a pre-check, verify the database and take a checksum of the contents.
            let old_digest = database_contents_digest(&path, rootfs)
                .context("pre-normalization contents check")?;

            // Perform normalization
            normalize_one(rootfs, &path, entry, timestamp)?;

            // Ensure that we haven't changed (or trashed) the database contents.
            let new_digest = database_contents_digest(&path, rootfs)
                .context("post-normalization contents check")?;
            if new_digest != old_digest {
                return Err(anyhow!("bdb normalization failed, detected content change"));
            }
        }

        Ok(())
    }

    fn write_zeros(file: &mut std::fs::File, length: impl Into<u64>) -> Result<(), anyhow::Error> {
        std::io::copy(&mut std::io::repeat(b'\x00').take(length.into()), file)?;
        Ok(())
    }

    // Verify a given BerkeleyDB database/file and then dump the internal contents into a hash function.
    // By checksumming the logical contents rather than the physical bytes on disk we can ensure that we
    // haven't actually changed anything.
    fn database_contents_digest(
        path: &PathBuf,
        rootfs: &openat::Dir,
    ) -> Result<[u8; 32], anyhow::Error> {
        // Build up the path we want and make sure it's a &str so Bubblewrap can use it.
        let path = PROC_SELF_CWD.join(path);
        let path = path
            .as_os_str()
            .to_str()
            .ok_or_else(|| anyhow!("bad path for bdb file"))?;

        // Run db_verify over the file, this tells us whether the actual BerkeleyDB code thinks it's
        // valid. db_verify will exit with a non-0 status if there are problems.
        let mut verify =
            Bubblewrap::new_with_mutability(rootfs, crate::ffi::BubblewrapMutability::Immutable)?;
        verify.append_child_argv(&["db_verify", "-q", path]);
        let cancellable = gio::Cancellable::new();
        verify.run_captured(Some(&cancellable))?;

        // Run db_dump, which will dump the contents of the database file in a transportable format,
        // and calculate the SHA256 digest of said contents. Since the contents are independent of whatever
        // random uninitialized data may lurk in the file itself it acts as a decent check of whether we've
        // inadvertently changed anything we shouldn't have.
        let mut dump =
            Bubblewrap::new_with_mutability(rootfs, crate::ffi::BubblewrapMutability::Immutable)?;
        dump.append_child_argv(&["db_dump", path]);
        let cancellable = gio::Cancellable::new();
        let digest = sha256(&dump.run_captured(Some(&cancellable))?);

        Ok(digest)
    }
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
