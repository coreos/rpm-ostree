//! High-level interface to retrieve host rpm-ostree history.
//!
//! The basic idea is that at deployment creation time, the upgrader does two
//! things: (1) it writes a GVariant file describing the deployment to
//! `/var/lib/rpm-ostree/history` and (2) it logs a journal message. These two
//! pieces are tied together through the deployment root timestamp (which is
//! used as the filename for the GVariant and is included in the message under
//! `DEPLOYMENT_TIMESTAMP`). Thus, we can retrieve the GVariant corresponding to
//! a specific journal message. See the upgrader code for more details.
//!
//! This journal message also includes the deployment path in `DEPLOYMENT_PATH`.
//! At boot time, `ostree-prepare-root` logs the resolved deployment path
//! in *its* message's `DEPLOYMENT_PATH` too. Thus, we can tie together boot
//! messages with their corresponding deployment messages. To do this, we do
//! something akin to the following:
//!
//!   - starting from the most recent journal entry, go backwards searching for
//!     OSTree boot messages
//!   - when a boot message is found, keep going backwards to find its matching
//!     rpm-ostree deploy message by comparing the two messages' deployment path
//!     fields
//!   - when a match is found, return a `HistoryEntry`
//!   - start up the search again for the next boot message
//!
//! There's some added complexity to deal with ordering between boot events and
//! deployment events, and some "reboot" squashing to yield a single
//! `HistoryEntry` if the system booted into the same deployment multiple times
//! in a row.
//!
//! The algorithm is streaming, i.e. it yields entries as it finds them, rather
//! than scanning the whole journal upfront. This can then be e.g. piped through
//! a pager, stopped after N entries, etc...

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::HistoryEntry;
use anyhow::{anyhow, Result};
use fn_error_context::context;
use openat::{self, Dir, SimpleType};
use std::collections::VecDeque;
use std::fs;
use std::ops::Deref;
use std::path::Path;
use systemd::journal::JournalRecord;

use openat_ext::OpenatDirExt;

#[cfg(test)]
use self::mock_journal as journal;
#[cfg(not(test))]
use systemd::journal;

// msg ostree-prepare-root emits at boot time when it resolved the deployment */
static OSTREE_BOOT_MSG: &str = "7170336a73ba4601bad31af888aa0df7";
// msg rpm-ostree emits when it creates the deployment */
static RPMOSTREE_DEPLOY_MSG: &str = "9bddbda177cd44d891b1b561a8a0ce9e";

static RPMOSTREE_HISTORY_DIR: &str = "/var/lib/rpm-ostree/history";

/// Context object used to iterate through `HistoryEntry` events.
pub struct HistoryCtx {
    journal: journal::Journal,
    marker_queue: VecDeque<Marker>,
    current_entry: Option<HistoryEntry>,
    search_mode: Option<JournalSearchMode>,
    reached_eof: bool,
}

// Markers are essentially deserialized journal messages, where all the
// interesting bits have been parsed out.

/// Marker for OSTree boot messages.
struct BootMarker {
    timestamp: u64,
    path: String,
    node: DevIno,
}

/// Marker for rpm-ostree deployment messages.
#[derive(Clone)]
struct DeploymentMarker {
    timestamp: u64,
    path: String,
    node: DevIno,
    cmdline: Option<String>,
}

enum Marker {
    Boot(BootMarker),
    Deployment(DeploymentMarker),
}

#[derive(Clone, PartialEq)]
struct DevIno {
    device: u64,
    inode: u64,
}

impl HistoryEntry {
    /// Create a new `HistoryEntry` from a boot marker and a deployment marker.
    fn new_from_markers(boot: BootMarker, deploy: DeploymentMarker) -> HistoryEntry {
        HistoryEntry {
            first_boot_timestamp: boot.timestamp,
            last_boot_timestamp: boot.timestamp,
            deploy_timestamp: deploy.timestamp,
            deploy_cmdline: deploy.cmdline.unwrap_or_default(),
            boot_count: 1,
            eof: false,
        }
    }

    fn eof() -> HistoryEntry {
        HistoryEntry {
            eof: true,
            first_boot_timestamp: 0,
            last_boot_timestamp: 0,
            deploy_timestamp: 0,
            deploy_cmdline: "".to_string(),
            boot_count: 0,
        }
    }
}

#[derive(PartialEq)]
enum JournalSearchMode {
    BootMsgs,
    BootAndDeploymentMsgs,
}

#[cfg(not(test))]
fn journal_record_timestamp(journal: &journal::Journal) -> Result<u64> {
    Ok(journal
        .timestamp()?
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs())
}

#[cfg(test)]
fn journal_record_timestamp(journal: &journal::Journal) -> Result<u64> {
    Ok(journal.current_timestamp.unwrap())
}

fn map_to_u64<T>(s: Option<&T>) -> Option<u64>
where
    T: Deref<Target = str>,
{
    s.and_then(|s| s.parse::<u64>().ok())
}

#[cfg(not(test))]
fn journal_open() -> Result<journal::Journal> {
    Ok(journal::OpenOptions::default()
        .system(true)
        .local_only(true)
        .runtime_only(false)
        .open()?)
}

#[cfg(test)]
fn journal_open() -> Result<mock_journal::Journal> {
    mock_journal::Journal::new()
}

fn history_get_oldest_deployment_msg_timestamp() -> Result<Option<u64>> {
    let mut journal = journal_open()?;
    journal.seek(journal::JournalSeek::Head)?;
    journal.match_add("MESSAGE_ID", RPMOSTREE_DEPLOY_MSG)?;
    while let Some(rec) = journal.next_entry()? {
        if let Some(ts) = map_to_u64(rec.get("DEPLOYMENT_TIMESTAMP")) {
            return Ok(Some(ts));
        }
    }
    Ok(None)
}

/// Gets the oldest deployment message in the journal, and nuke all the GVariant data files
/// that correspond to deployments older than that one. Essentially, this binds pruning to
/// journal pruning.
#[context("Failed to prune history")]
pub(crate) fn history_prune() -> CxxResult<()> {
    if !Path::new(RPMOSTREE_HISTORY_DIR).exists() {
        return Ok(());
    }
    let oldest_timestamp = history_get_oldest_deployment_msg_timestamp()?;

    // Cleanup any entry older than the oldest entry in the journal. Also nuke anything else that
    // doesn't belong here; we own this dir.
    let dir = Dir::open(RPMOSTREE_HISTORY_DIR)?;
    for entry in dir.list_dir(".")? {
        let entry = entry?;
        let ftype = dir.get_file_type(&entry)?;

        let fname = entry.file_name();
        if let Some(oldest_ts) = oldest_timestamp {
            if ftype == SimpleType::File {
                if let Some(ts) = map_to_u64(fname.to_str().as_ref()) {
                    if ts >= oldest_ts {
                        continue;
                    }
                }
            }
        }

        if ftype == SimpleType::Dir {
            fs::remove_dir_all(Path::new(RPMOSTREE_HISTORY_DIR).join(fname))?;
        } else {
            dir.remove_file(fname)?;
        }
    }

    Ok(())
}

pub(crate) fn history_ctx_new() -> CxxResult<Box<HistoryCtx>> {
    Ok(HistoryCtx::new_boxed()?)
}

impl HistoryCtx {
    /// Create a new context object.
    fn new_boxed() -> Result<Box<HistoryCtx>> {
        let mut journal = journal_open()?;
        journal.seek(journal::JournalSeek::Tail)?;

        Ok(Box::new(HistoryCtx {
            journal,
            marker_queue: VecDeque::new(),
            current_entry: None,
            search_mode: None,
            reached_eof: false,
        }))
    }

    /// Ensures the journal filters are set up for the messages we're interested in.
    fn set_search_mode(&mut self, mode: JournalSearchMode) -> Result<()> {
        if Some(&mode) != self.search_mode.as_ref() {
            self.journal.match_flush()?;
            self.journal.match_add("MESSAGE_ID", OSTREE_BOOT_MSG)?;
            if mode == JournalSearchMode::BootAndDeploymentMsgs {
                self.journal.match_add("MESSAGE_ID", RPMOSTREE_DEPLOY_MSG)?;
            }
            self.search_mode = Some(mode);
        }
        Ok(())
    }

    /// Creates a marker from an OSTree boot message. Uses the timestamp of the message
    /// itself as the boot time. Returns None if record is incomplete.
    fn boot_record_to_marker(&self, record: &JournalRecord) -> Result<Option<Marker>> {
        if let (Some(path), Some(device), Some(inode)) = (
            record.get("DEPLOYMENT_PATH"),
            map_to_u64(record.get("DEPLOYMENT_DEVICE")),
            map_to_u64(record.get("DEPLOYMENT_INODE")),
        ) {
            return Ok(Some(Marker::Boot(BootMarker {
                timestamp: journal_record_timestamp(&self.journal)?,
                path: path.clone(),
                node: DevIno { device, inode },
            })));
        }
        Ok(None)
    }

    /// Creates a marker from an rpm-ostree deploy message. Uses the `DEPLOYMENT_TIMESTAMP`
    /// in the message as the deploy time. This matches the history gv filename for that
    /// deployment. Returns None if record is incomplete.
    fn deployment_record_to_marker(&self, record: &JournalRecord) -> Result<Option<Marker>> {
        if let (Some(timestamp), Some(device), Some(inode), Some(path)) = (
            map_to_u64(record.get("DEPLOYMENT_TIMESTAMP")),
            map_to_u64(record.get("DEPLOYMENT_DEVICE")),
            map_to_u64(record.get("DEPLOYMENT_INODE")),
            record.get("DEPLOYMENT_PATH"),
        ) {
            return Ok(Some(Marker::Deployment(DeploymentMarker {
                timestamp,
                node: DevIno { device, inode },
                path: path.clone(),
                cmdline: record.get("COMMAND_LINE").cloned(),
            })));
        }
        Ok(None)
    }

    /// Goes to the next OSTree boot msg in the journal and returns its marker.
    fn find_next_boot_marker(&mut self) -> Result<Option<BootMarker>> {
        self.set_search_mode(JournalSearchMode::BootMsgs)?;
        while let Some(rec) = self.journal.previous_entry()? {
            if let Some(Marker::Boot(m)) = self.boot_record_to_marker(&rec)? {
                return Ok(Some(m));
            }
        }
        Ok(None)
    }

    /// Returns a marker of the appropriate kind for a given journal message.
    fn record_to_marker(&self, record: &JournalRecord) -> Result<Option<Marker>> {
        Ok(match record.get("MESSAGE_ID").unwrap() {
            m if m == OSTREE_BOOT_MSG => self.boot_record_to_marker(record)?,
            m if m == RPMOSTREE_DEPLOY_MSG => self.deployment_record_to_marker(record)?,
            m => panic!("matched an unwanted message: {:?}", m),
        })
    }

    /// Goes to the next OSTree boot or rpm-ostree deploy msg in the journal, creates a
    /// marker for it, and returns it.
    fn find_next_marker(&mut self) -> Result<Option<Marker>> {
        self.set_search_mode(JournalSearchMode::BootAndDeploymentMsgs)?;
        while let Some(rec) = self.journal.previous_entry()? {
            if let Some(marker) = self.record_to_marker(&rec)? {
                return Ok(Some(marker));
            }
        }
        Ok(None)
    }

    /// Finds the matching deployment marker for the next boot marker in the queue.
    fn scan_until_path_match(&mut self) -> Result<Option<(BootMarker, DeploymentMarker)>> {
        // keep popping & scanning until we get to the next boot marker
        let boot_marker = loop {
            match self.marker_queue.pop_front() {
                Some(Marker::Boot(m)) => break m,
                Some(Marker::Deployment(_)) => continue,
                None => match self.find_next_boot_marker()? {
                    Some(m) => break m,
                    None => return Ok(None),
                },
            }
        };

        // check if its corresponding deployment is already in the queue
        for marker in self.marker_queue.iter() {
            if let Marker::Deployment(m) = marker {
                if m.path == boot_marker.path {
                    return Ok(Some((boot_marker, m.clone())));
                }
            }
        }

        // keep collecting until we get a matching path
        while let Some(marker) = self.find_next_marker()? {
            self.marker_queue.push_back(marker);
            // ...and now borrow it back; might be a cleaner way to do this
            let marker = self.marker_queue.back().unwrap();

            if let Marker::Deployment(m) = marker {
                if m.path == boot_marker.path {
                    return Ok(Some((boot_marker, m.clone())));
                }
            }
        }

        Ok(None)
    }

    /// Returns the next history entry, which consists of a boot timestamp and its matching
    /// deploy timestamp.
    fn scan_until_next_entry(&mut self) -> Result<Option<HistoryEntry>> {
        while let Some((boot_marker, deployment_marker)) = self.scan_until_path_match()? {
            if boot_marker.node != deployment_marker.node {
                // This is a non-foolproof safety valve to ensure that the boot is definitely
                // referring to the matched up deployment. E.g. if the correct, more recent,
                // matching deployment somehow had its journal entry lost, we don't want to report
                // this boot with the wrong match. For now, just silently skip over that boot. No
                // history is better than wrong history. In the future, we could consider printing
                // this somehow too.
                continue;
            }
            return Ok(Some(HistoryEntry::new_from_markers(
                boot_marker,
                deployment_marker,
            )));
        }
        Ok(None)
    }

    /// Returns the next *new* entry. This essentially collapses multiple subsequent boots
    /// of the same deployment into a single entry. The `boot_count` field represents the
    /// number of boots squashed, and `*_boot_timestamp` fields provide the timestamp of the
    /// first and last boots.
    fn scan_until_next_new_entry(&mut self) -> Result<Option<HistoryEntry>> {
        while let Some(entry) = self.scan_until_next_entry()? {
            if self.current_entry.is_none() {
                /* first scan ever; prime with first entry */
                self.current_entry.replace(entry);
                continue;
            }

            let current_deploy_timestamp = self.current_entry.as_ref().unwrap().deploy_timestamp;
            if entry.deploy_timestamp == current_deploy_timestamp {
                /* we found an older boot for the same deployment: update first boot */
                let current_entry = &mut self.current_entry.as_mut().unwrap();
                current_entry.first_boot_timestamp = entry.first_boot_timestamp;
                current_entry.boot_count += 1;
            } else {
                /* found a new boot for a different deployment; flush out current one */
                return Ok(self.current_entry.replace(entry));
            }
        }

        /* flush out final entry if any */
        Ok(self.current_entry.take())
    }

    /// Returns the next entry. This is a thin wrapper around `scan_until_next_new_entry`
    /// that mostly just handles the `Option` -> EOF conversion for the C side.
    pub(crate) fn next_entry(&mut self) -> CxxResult<HistoryEntry> {
        if self.reached_eof {
            return Err(anyhow!("next_entry() called after having reached EOF!").into());
        }

        match self.scan_until_next_new_entry()? {
            Some(e) => Ok(e),
            None => {
                self.reached_eof = true;
                Ok(HistoryEntry::eof())
            }
        }
    }
}

/// A minimal mock journal interface so we can unit test various code paths without adding
/// stuff in the host journal; in fact without needing any system journal access at all.
#[cfg(test)]
mod mock_journal {
    use super::Result;
    pub use systemd::journal::{JournalRecord, JournalSeek};

    pub struct Journal {
        pub entries: Vec<(u64, JournalRecord)>,
        pub current_timestamp: Option<u64>,
        msg_ids: Vec<String>,
    }

    impl Journal {
        pub fn new() -> Result<Journal> {
            Ok(Journal {
                entries: Vec::new(),
                current_timestamp: None,
                msg_ids: Vec::new(),
            })
        }
        pub fn seek(&mut self, _: JournalSeek) -> Result<()> {
            Ok(())
        }
        pub fn match_flush(&mut self) -> Result<()> {
            self.msg_ids.clear();
            Ok(())
        }
        pub fn match_add(&mut self, _: &str, msg_id: &str) -> Result<()> {
            self.msg_ids.push(msg_id.into());
            Ok(())
        }
        pub fn previous_entry(&mut self) -> Result<Option<JournalRecord>> {
            while let Some((timestamp, record)) = self.entries.pop() {
                if self.msg_ids.contains(record.get("MESSAGE_ID").unwrap()) {
                    self.current_timestamp = Some(timestamp);
                    return Ok(Some(record));
                }
            }
            Ok(None)
        }
        // This is only used by the prune path, which we're not unit testing.
        pub fn next_entry(&mut self) -> Result<Option<JournalRecord>> {
            unimplemented!();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    impl HistoryCtx {
        fn add_boot_record_inode(&mut self, ts: u64, path: &str, inode: u64) {
            if let Some(entry) = self.journal.entries.last() {
                assert!(ts > entry.0);
            }
            let mut record = JournalRecord::new();
            record.insert("MESSAGE_ID".into(), OSTREE_BOOT_MSG.into());
            record.insert("DEPLOYMENT_PATH".into(), path.into());
            record.insert("DEPLOYMENT_DEVICE".into(), inode.to_string());
            record.insert("DEPLOYMENT_INODE".into(), inode.to_string());
            self.journal.entries.push((ts, record));
        }

        fn add_boot_record(&mut self, ts: u64, path: &str) {
            self.add_boot_record_inode(ts, path, 0);
        }

        fn add_deployment_record_inode(&mut self, ts: u64, path: &str, inode: u64) {
            if let Some(entry) = self.journal.entries.last() {
                assert!(ts > entry.0);
            }
            let mut record = JournalRecord::new();
            record.insert("MESSAGE_ID".into(), RPMOSTREE_DEPLOY_MSG.into());
            record.insert("DEPLOYMENT_TIMESTAMP".into(), ts.to_string());
            record.insert("DEPLOYMENT_PATH".into(), path.into());
            record.insert("DEPLOYMENT_DEVICE".into(), inode.to_string());
            record.insert("DEPLOYMENT_INODE".into(), inode.to_string());
            self.journal.entries.push((ts, record));
        }

        fn add_deployment_record(&mut self, ts: u64, path: &str) {
            self.add_deployment_record_inode(ts, path, 0);
        }

        fn assert_next_entry(
            &mut self,
            first_boot_timestamp: u64,
            last_boot_timestamp: u64,
            deploy_timestamp: u64,
            boot_count: u64,
        ) {
            assert!(
                self.next_entry().unwrap()
                    == HistoryEntry {
                        first_boot_timestamp: first_boot_timestamp,
                        last_boot_timestamp: last_boot_timestamp,
                        deploy_timestamp: deploy_timestamp,
                        deploy_cmdline: "".to_string(),
                        boot_count: boot_count,
                        eof: false,
                    }
            );
        }

        fn assert_eof(&mut self) {
            assert!(self.next_entry().unwrap().eof);
        }
    }

    #[test]
    fn basic() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        assert!(ctx.next_entry().unwrap().eof);
        assert!(ctx.next_entry().is_err());
    }

    #[test]
    fn basic_deploy() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.assert_eof();
    }

    #[test]
    fn basic_boot() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_boot_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.assert_eof();
    }

    #[test]
    fn basic_match() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(1, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.assert_next_entry(1, 1, 0, 1);
        ctx.assert_eof();
    }

    #[test]
    fn multi_boot() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_boot_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(1, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_boot_record(3, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(4, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(5, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(6, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(7, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.assert_next_entry(5, 7, 4, 3);
        ctx.assert_eof();
    }

    #[test]
    fn multi_deployment() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(1, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(2, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(3, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_boot_record(4, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(5, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(6, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.assert_next_entry(4, 4, 2, 1);
        ctx.assert_eof();
    }

    #[test]
    fn multi1() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(1, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(2, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(3, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(4, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_boot_record(5, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_boot_record(6, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.assert_next_entry(5, 6, 4, 2);
        ctx.assert_next_entry(1, 3, 0, 3);
        ctx.assert_eof();
    }

    #[test]
    fn multi2() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(1, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_deployment_record(2, "/ostree/deploy/fedora/deploy/deadcafe.2");
        ctx.add_deployment_record(3, "/ostree/deploy/fedora/deploy/deadcafe.3");
        ctx.add_deployment_record(4, "/ostree/deploy/fedora/deploy/deadcafe.4");
        ctx.add_boot_record(5, "/ostree/deploy/fedora/deploy/deadcafe.4");
        ctx.add_boot_record(6, "/ostree/deploy/fedora/deploy/deadcafe.3");
        ctx.add_boot_record(7, "/ostree/deploy/fedora/deploy/deadcafe.2");
        ctx.add_boot_record(8, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_boot_record(9, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.assert_next_entry(9, 9, 0, 1);
        ctx.assert_next_entry(8, 8, 1, 1);
        ctx.assert_next_entry(7, 7, 2, 1);
        ctx.assert_next_entry(6, 6, 3, 1);
        ctx.assert_next_entry(5, 5, 4, 1);
        ctx.assert_eof();
    }

    #[test]
    fn multi3() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(1, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_deployment_record(2, "/ostree/deploy/fedora/deploy/deadcafe.2");
        ctx.add_deployment_record(3, "/ostree/deploy/fedora/deploy/deadcafe.3");
        ctx.add_deployment_record(4, "/ostree/deploy/fedora/deploy/deadcafe.4");
        ctx.add_boot_record(5, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(6, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_boot_record(7, "/ostree/deploy/fedora/deploy/deadcafe.2");
        ctx.add_boot_record(8, "/ostree/deploy/fedora/deploy/deadcafe.3");
        ctx.add_boot_record(9, "/ostree/deploy/fedora/deploy/deadcafe.4");
        ctx.assert_next_entry(9, 9, 4, 1);
        ctx.assert_next_entry(8, 8, 3, 1);
        ctx.assert_next_entry(7, 7, 2, 1);
        ctx.assert_next_entry(6, 6, 1, 1);
        ctx.assert_next_entry(5, 5, 0, 1);
        ctx.assert_eof();
    }

    #[test]
    fn multi4() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(1, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(2, "/ostree/deploy/fedora/deploy/deadcafe.2");
        ctx.add_boot_record(3, "/ostree/deploy/fedora/deploy/deadcafe.2");
        ctx.add_deployment_record(4, "/ostree/deploy/fedora/deploy/deadcafe.4");
        ctx.add_boot_record(5, "/ostree/deploy/fedora/deploy/deadcafe.4");
        ctx.add_deployment_record(6, "/ostree/deploy/fedora/deploy/deadcafe.6");
        ctx.add_boot_record(7, "/ostree/deploy/fedora/deploy/deadcafe.6");
        ctx.add_deployment_record(8, "/ostree/deploy/fedora/deploy/deadcafe.8");
        ctx.add_boot_record(9, "/ostree/deploy/fedora/deploy/deadcafe.8");
        ctx.assert_next_entry(9, 9, 8, 1);
        ctx.assert_next_entry(7, 7, 6, 1);
        ctx.assert_next_entry(5, 5, 4, 1);
        ctx.assert_next_entry(3, 3, 2, 1);
        ctx.assert_next_entry(1, 1, 0, 1);
        ctx.assert_eof();
    }

    #[test]
    fn multi5() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record(0, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_deployment_record(1, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_boot_record(2, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(3, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(4, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(5, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_boot_record(6, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.add_boot_record(7, "/ostree/deploy/fedora/deploy/deadcafe.0");
        ctx.add_boot_record(8, "/ostree/deploy/fedora/deploy/deadcafe.1");
        ctx.assert_next_entry(8, 8, 1, 1);
        ctx.assert_next_entry(7, 7, 0, 1);
        ctx.assert_next_entry(5, 6, 1, 2);
        ctx.assert_next_entry(2, 4, 0, 3);
        ctx.assert_eof();
    }

    #[test]
    fn inode1() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record_inode(0, "/ostree/deploy/fedora/deploy/deadcafe.0", 1000);
        ctx.add_deployment_record_inode(1, "/ostree/deploy/fedora/deploy/deadcafe.1", 2000);
        ctx.add_boot_record_inode(2, "/ostree/deploy/fedora/deploy/deadcafe.0", 1000);
        ctx.add_boot_record_inode(3, "/ostree/deploy/fedora/deploy/deadcafe.1", 2000);
        ctx.assert_next_entry(3, 3, 1, 1);
        ctx.assert_next_entry(2, 2, 0, 1);
        ctx.assert_eof();
    }

    #[test]
    fn inode2() {
        let mut ctx = HistoryCtx::new_boxed().unwrap();
        ctx.add_deployment_record_inode(0, "/ostree/deploy/fedora/deploy/deadcafe.0", 1000);
        ctx.add_deployment_record_inode(1, "/ostree/deploy/fedora/deploy/deadcafe.1", 2000);
        ctx.add_boot_record_inode(2, "/ostree/deploy/fedora/deploy/deadcafe.1", 2000);
        ctx.add_boot_record_inode(3, "/ostree/deploy/fedora/deploy/deadcafe.0", 1000);
        ctx.add_boot_record_inode(4, "/ostree/deploy/fedora/deploy/deadcafe.0", 1001);
        ctx.assert_next_entry(3, 3, 0, 1);
        ctx.assert_next_entry(2, 2, 1, 1);
        ctx.assert_eof();
    }
}
