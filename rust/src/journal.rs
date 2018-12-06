/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

extern crate systemd;

use failure::Fallible;
use self::systemd::id128::Id128;
use self::systemd::journal;

static OSTREE_FINALIZE_STAGED_SERVICE: &'static str = "ostree-finalize-staged.service";
static OSTREE_DEPLOYMENT_FINALIZING_MSG_ID: &'static str = "e8646cd63dff4625b77909a8e7a40994";
static OSTREE_DEPLOYMENT_COMPLETE_MSG_ID: &'static str = "dd440e3e549083b63d0efc7dc15255f1";

fn print_staging_failure_msg(msg: Option<&str>) -> Fallible<()> {
    println!("Warning: failed to finalize previous deployment");
    if let Some(msg) = msg {
        println!("         {}", msg);
    }
    println!(
        "         check `journalctl -b -1 -u {}`",
        OSTREE_FINALIZE_STAGED_SERVICE
    );
    return Ok(()); // for convenience
}

/// Look for a failure from ostree-finalized-stage.service in the journal of the previous boot.
fn journal_print_staging_failure() -> Fallible<()> {
    let mut j = journal::Journal::open(journal::JournalFiles::System, false, true)?;

    // first, go to the first entry of the current boot
    let boot_id = Id128::from_boot()?;
    j.match_add("_BOOT_ID", boot_id.to_string().as_str())?;
    j.seek(journal::JournalSeek::Head)?;
    j.match_flush()?;

    // Now, go backwards until we hit the first entry from the previous boot. In theory that should
    // just be a single `sd_journal_previous()` call, but we need a loop here, see:
    // https://github.com/systemd/systemd/commit/dc00966228ff90c554fd034e588ea55eb605ec52
    let mut previous_boot_id: Id128 = boot_id.clone();
    while previous_boot_id == boot_id {
        match j.previous_record()? {
            Some(_) => previous_boot_id = j.monotonic_timestamp()?.1,
            None => return Ok(()), // no previous boot!
        }
    }
    // we just need it as a string from now on
    let previous_boot_id = previous_boot_id.to_string();
    let final_cursor_from_previous_boot = j.cursor()?;

    // look for OSTree's finalization msg
    // NB: we avoid using _SYSTEMD_UNIT here because it's not fully reliable (at least on el7)
    // see: https://github.com/systemd/systemd/issues/2913
    j.match_add("MESSAGE_ID", OSTREE_DEPLOYMENT_FINALIZING_MSG_ID)?;
    j.match_add("SYSLOG_IDENTIFIER", "ostree")?;
    j.match_add("_BOOT_ID", previous_boot_id.as_str())?;
    let ostree_pid = match j.previous_record()? {
        None => return Ok(()), // didn't run (or staged deployment was cleaned up)
        Some(mut rec) => rec.remove("_PID").unwrap(),
    };

    // now we're at the finalizing msg, remember its position
    let finalizing_cursor = j.cursor()?;

    // and now check if it actually completed the transaction
    j.match_flush()?;
    j.match_add("MESSAGE_ID", OSTREE_DEPLOYMENT_COMPLETE_MSG_ID)?;
    j.match_add("SYSLOG_IDENTIFIER", "ostree")?;
    j.match_add("_PID", ostree_pid.as_str())?;
    j.match_add("_BOOT_ID", previous_boot_id.as_str())?;

    if j.next_record()? != None {
        return Ok(()); // finished successfully!
    }

    // OK, there was a failure; go back to finalizing msg before digging deeper
    j.match_flush()?;
    j.seek(journal::JournalSeek::Cursor {
        cursor: finalizing_cursor,
    })?;

    // Try to find the 'Failed with result' msg from systemd. This is a bit brittle right now until
    // we get a proper structured msg (see https://github.com/systemd/systemd/issues/10265).
    j.match_add("UNIT", OSTREE_FINALIZE_STAGED_SERVICE)?;
    j.match_add("_COMM", "systemd")?;
    j.match_add("_BOOT_ID", previous_boot_id.as_str())?;

    let mut exited = false;
    while let Some(rec) = j.next_record()? {
        if let Some(msg) = rec.get("MESSAGE") {
            if msg.contains("Failed with result") || msg.contains("control process exited") {
                if !msg.contains("exit-code") && !msg.contains("code=exited") {
                    /* just print that msg; e.g. could be timeout, signal, core-dump */
                    return print_staging_failure_msg(Some(msg));
                }
                exited = true;
                break;
            }
        }
    }

    if !exited {
        /* even systemd doesn't know what happened? OK, just stick with unknown */
        return print_staging_failure_msg(None);
    }

    // go to the last entry for that boot so we search backwards from there
    j.match_flush()?;
    j.seek(journal::JournalSeek::Cursor {
        cursor: final_cursor_from_previous_boot,
    })?;

    // just find the last msg from ostree, it's probably an error msg
    j.match_add("_BOOT_ID", previous_boot_id.as_str())?;
    j.match_add("SYSLOG_IDENTIFIER", "ostree")?;
    j.match_add("_PID", ostree_pid.as_str())?;
    // otherwise we risk matching the finalization msg (journal transport), which on el7
    // systemd can be timestamped *after* the error msg (stdout transport)...
    j.match_add("_TRANSPORT", "stdout")?;

    if let Some(rec) = j.previous_record()? {
        if let Some(msg) = rec.get("MESSAGE") {
            // only print the msg if it starts with 'error:', otherwise some other really
            // weird thing happened that might span multiple lines?
            if msg.starts_with("error:") {
                return print_staging_failure_msg(Some(msg));
            }
        }
    }

    return print_staging_failure_msg(None);
}

mod ffi {
    use super::*;
    use crate::ffiutil::*;
    use glib_sys;
    use libc;
    #[no_mangle]
    pub extern "C" fn ror_journal_print_staging_failure(
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        int_glib_error(journal_print_staging_failure(), gerror)
    }
}
pub use self::ffi::*;
