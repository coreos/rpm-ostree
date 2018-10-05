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

use self::systemd::id128::Id128;
use self::systemd::journal;
use std::io;

static OSTREE_FINALIZE_STAGED_SERVICE: &'static str = "ostree-finalize-staged.service";
static OSTREE_DEPLOYMENT_FINALIZING_MSG_ID: &'static str = "e8646cd63dff4625b77909a8e7a40994";
static OSTREE_DEPLOYMENT_COMPLETE_MSG_ID: &'static str = "dd440e3e549083b63d0efc7dc15255f1";

/// Look for a failure from ostree-finalized-stage.service in the journal of the previous boot.
pub fn journal_find_staging_failure() -> io::Result<bool> {
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
            None => return Ok(false), // no previous boot!
        }
    }
    // we just need it as a string from now on
    let previous_boot_id = previous_boot_id.to_string();

    // look for OSTree's finalization msg
    j.match_add("MESSAGE_ID", OSTREE_DEPLOYMENT_FINALIZING_MSG_ID)?;
    j.match_add("_SYSTEMD_UNIT", OSTREE_FINALIZE_STAGED_SERVICE)?;
    j.match_add("_BOOT_ID", previous_boot_id.as_str())?;
    if j.previous_record()? == None {
        return Ok(false); // didn't run (or staged deployment was cleaned up)
    }

    // and now check if it actually completed the transaction
    j.match_flush()?;
    j.match_add("MESSAGE_ID", OSTREE_DEPLOYMENT_COMPLETE_MSG_ID)?;
    j.match_add("_SYSTEMD_UNIT", OSTREE_FINALIZE_STAGED_SERVICE)?;
    j.match_add("_BOOT_ID", previous_boot_id.as_str())?;

    Ok(j.next_record()? == None)
}
