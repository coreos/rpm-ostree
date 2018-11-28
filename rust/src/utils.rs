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

use failure::Fallible;
use std::collections::HashMap;
use std::io::prelude::*;
use std::{fs, io};
use tempfile;

use curl::easy::Easy;

fn download_url_to_tmpfile(url: &str) -> Fallible<fs::File> {
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

/// Given an input string `s`, replace variables of the form `${foo}` with
/// values provided in `vars`.  No quoting syntax is available, so it is
/// not possible to have a literal `${` in the string.
pub fn varsubst(instr: &str, vars: &HashMap<String, String>) -> Fallible<String> {
    let mut buf = instr;
    let mut s = "".to_string();
    while buf.len() > 0 {
        if let Some(start) = buf.find("${") {
            let (prefix, rest) = buf.split_at(start);
            let rest = &rest[2..];
            s.push_str(prefix);
            if let Some(end) = rest.find("}") {
                let (varname, remainder) = rest.split_at(end);
                let remainder = &remainder[1..];
                if let Some(val) = vars.get(varname) {
                    s.push_str(val);
                } else {
                    bail!("Unknown variable reference ${{{}}}", varname);
                }
                buf = remainder;
            } else {
                bail!("Unclosed variable");
            }
        } else {
            s.push_str(buf);
            break;
        }
    }
    return Ok(s);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn subs() -> HashMap<String, String> {
        let mut h = HashMap::new();
        h.insert("basearch".to_string(), "ppc64le".to_string());
        h.insert("osvendor".to_string(), "fedora".to_string());
        h
    }

    fn test_noop(s: &str, subs: &HashMap<String, String>) {
        let r = varsubst(s, subs).unwrap();
        assert_eq!(r, s);
    }

    #[test]
    fn varsubst_noop() {
        let subs = subs();
        test_noop("", &subs);
        test_noop("foo bar baz", &subs);
        test_noop("}", &subs);
        test_noop("$} blah$ }}", &subs);
    }

    #[test]
    fn varsubsts() {
        let subs = subs();
        let r = varsubst(
            "ostree/${osvendor}/${basearch}/blah/${basearch}/whee",
            &subs,
        ).unwrap();
        assert_eq!(r, "ostree/fedora/ppc64le/blah/ppc64le/whee");
        let r = varsubst("${osvendor}${basearch}", &subs).unwrap();
        assert_eq!(r, "fedorappc64le");
        let r = varsubst("${osvendor}", &subs).unwrap();
        assert_eq!(r, "fedora");
    }
}

mod ffi {
    use super::*;
    use ffiutil::*;
    use glib;
    use glib_sys;
    use libc;
    use std::ffi::CString;
    use std::os::unix::io::IntoRawFd;
    use std::ptr;

    #[no_mangle]
    pub extern "C" fn ror_download_to_fd(
        url: *const libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let url = ffi_view_nullable_str(url).unwrap();
        match download_url_to_tmpfile(url) {
            Ok(f) => f.into_raw_fd(),
            Err(e) => {
                error_to_glib(&e, gerror);
                -1
            }
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_util_varsubst(
        s: *const libc::c_char,
        h: *mut glib_sys::GHashTable,
        gerror: *mut *mut glib_sys::GError,
    ) -> *mut libc::c_char {
        let s = ffi_view_nullable_str(s).unwrap();
        let h_rs: HashMap<String, String> =
            unsafe { glib::translate::FromGlibPtrContainer::from_glib_none(h) };
        match varsubst(s, &h_rs) {
            Ok(s) => CString::new(s).unwrap().into_raw(),
            Err(e) => {
                error_to_glib(&e, gerror);
                ptr::null_mut()
            }
        }
    }
}
pub use self::ffi::*;
