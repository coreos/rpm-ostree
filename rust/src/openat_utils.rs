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

use std::{fs, io};
use openat;

/// Helper functions for openat::Dir
pub(crate) trait OpenatDirExt {
    // IMO should propose this at least in a "utils" bit of the openat crate;
    // Like 95% of the time I'm looking at errno (with files) it's for ENOENT,
    // and Rust has an elegant way to map that with Option<>.  Every other
    // error I usually just want to propagate back up.
    fn open_file_optional<P: openat::AsPath>(&self, p: P) -> io::Result<Option<fs::File>>;
}

impl OpenatDirExt for openat::Dir {
    fn open_file_optional<P: openat::AsPath>(&self, p: P) -> io::Result<Option<fs::File>> {
        match self.open_file(p) {
            Ok(f) => Ok(Some(f)),
            Err(e) => {
                if e.kind() == io::ErrorKind::NotFound {
                    Ok(None)
                } else {
                    Err(e)
                }
            }
        }
    }
}

