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

use std::io::prelude::*;
use std::{fs, io};
use tempfile;

use curl::easy::Easy;

fn download_url_to_tmpfile(url: &str) -> io::Result<fs::File> {
    let mut tmpf = tempfile::tempfile()?;
    {
        let mut output = io::BufWriter::new(&mut tmpf);
        let mut handle = Easy::new();
        handle.follow_location(true)?;
        handle.fail_on_error(true)?;
        handle.url(url)?;

        let mut transfer = handle.transfer();
        transfer.write_function(|data| output.write_all(data).and(Ok(data.len())).or(Ok(0)))?;
        transfer.perform()?;
    }

    tmpf.seek(io::SeekFrom::Start(0))?;
    Ok(tmpf)
}

mod ffi {
    use super::*;
    use glib_sys;
    use libc;
    use std::os::unix::io::IntoRawFd;

    #[no_mangle]
    pub extern "C" fn ror_download_to_fd(
        url: *const libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        use ffiutil::*;
        let url = str_from_nullable(url).unwrap();
        match download_url_to_tmpfile(url) {
            Ok(f) => f.into_raw_fd(),
            Err(e) => {
                error_to_glib(&e, gerror);
                -1
            }
        }
    }
}
pub use self::ffi::*;
