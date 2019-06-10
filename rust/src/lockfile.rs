/*
 * Copyright (C) 2019 Red Hat, Inc.
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

/* Copied and adapted from: treefile.rs
 */

use failure::Fallible;
use serde_derive::{Deserialize, Serialize};
use serde_json;
use std::collections::HashMap;
use std::path::Path;
use std::io;

use crate::utils;

/// Parse a JSON lockfile definition.
fn lockfile_parse_stream<R: io::Read>(input: &mut R,) -> Fallible<LockfileConfig> {
    let lockfile: LockfileConfig = serde_json::from_reader(input).map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("serde-json: {}", e.to_string()),
        )
    })?;
    Ok(lockfile)
}

/// Given a lockfile filename, parse it
fn lockfile_parse<P: AsRef<Path>>(filename: P,) -> Fallible<LockfileConfig> {
    let filename = filename.as_ref();
    let mut f = io::BufReader::new(utils::open_file(filename)?);
    filename.file_name().map(|s| s.to_string_lossy()).ok_or_else(
        || io::Error::new(io::ErrorKind::InvalidInput, "Expected a filename"))?;
    let lf = lockfile_parse_stream(&mut f).map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("Parsing {}: {}", filename.to_string_lossy(), e.to_string()),
        )
    })?;
    Ok(lf)
}

/// Lockfile format:
/// ```
/// {
///    "packages": [
///        [
///          "NEVRA1",
///          "<digest-algo>:<digest>"
///        ],
///        [
///          "NEVRA2",
///          "<digest-algo>:<digest>"
///        ],
///        ...
///    ]
/// }
/// ```
#[derive(Serialize, Deserialize, Debug)]
#[serde(deny_unknown_fields)]
struct LockfileConfig {
    // Core content
    packages: Vec<(String, String)>,
}

#[cfg(test)]
mod tests {
    use super::*;

    static VALID_PRELUDE_JS: &str = r###"
{
 "packages": [["foo", "repodata-foo"], ["bar", "repodata-bar"]]
}
"###;

    static INVALID_PRELUDE_JS: &str = r###"
{
 "packages": [["foo", "repodata-foo"], ["bar", "repodata-bar"]],
 "packages-x86_64": [["baz", "repodata-baz"]],
 "packages-comment": "comment here",
}
"###;

    #[test]
    fn basic_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE_JS.as_bytes());
        let lockfile =
            lockfile_parse_stream(&mut input).unwrap();
        assert!(lockfile.packages.len() == 2);
    }

    #[test]
    fn test_invalid() {
        let mut input = io::BufReader::new(INVALID_PRELUDE_JS.as_bytes());
        match lockfile_parse_stream(&mut input) {
            Err(ref e) => match e.downcast_ref::<io::Error>() {
                Some(ref ioe) if ioe.kind() == io::ErrorKind::InvalidInput => {}
                _ => panic!("Expected invalid lockfile, not {}", e.to_string()),
            },
            Ok(_) => panic!("Expected invalid lockfile"),
        }
    }

    #[test]
    fn test_open_file_nonexistent() {
        let path = "/usr/share/empty/manifest.json";
        match lockfile_parse(path) {
            Err(ref e) => assert!(e
                .to_string()
                .starts_with(format!("Can't open file {:?}:", path).as_str())),
            Ok(_) => panic!("Expected nonexistent lockfile error for {}", path),
        }
    }
}

mod ffi {
    use super::*;
    use glib_sys;
    use glib::translate::*;
    use libc;
    use std::ptr;

    use crate::ffiutil::*;

    #[no_mangle]
    pub extern "C" fn ror_lockfile_get_map(
        filename: *const libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> *mut glib_sys::GHashTable {
        // Convert arguments
        let filename = ffi_view_os_str(filename);
        match lockfile_parse(filename) {
            Err(ref e) => {
                error_to_glib(e, gerror);
                ptr::null_mut()
            },
            Ok(mut lockfile) => {
                let map = lockfile.packages
                    .drain(..)
                    .fold(HashMap::<String, String>::new(), |mut acc, (k, v)| {
                        acc.insert(k, v);
                        acc
                    }
                );
                map.to_glib_full()
            }
        }
    }
}
pub use self::ffi::*;
