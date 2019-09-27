/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use failure::{bail, Fallible};
use failure::ResultExt;
use std::collections::HashMap;
use std::io::prelude::*;
use std::path::Path;
use std::{fs, io};
use tempfile;

use curl::easy::Easy;

#[derive(PartialEq)]
/// Supported config serialization used by treefile and lockfile
pub enum InputFormat {
    YAML,
    JSON,
}

impl InputFormat {
    pub fn detect_from_filename<P: AsRef<Path>>(filename: P) -> Fallible<Self> {
        let filename = filename.as_ref();
        let basename = filename
            .file_name()
            .map(|s| s.to_string_lossy())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "Expected a filename"))?;
        if basename.ends_with(".yaml") || basename.ends_with(".yml") {
            Ok(Self::YAML)
        } else {
            Ok(Self::JSON)
        }
    }
}

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

/// Open file for reading and provide context containing filename on failures.
pub fn open_file<P: AsRef<Path>>(filename: P) -> Fallible<fs::File> {
    return Ok(fs::File::open(filename.as_ref())
        .with_context(|e| format!("Can't open file {:?} for reading: {}", filename.as_ref().display(), e))?);
}

/// Open file for writing and provide context containing filename on failures.
pub fn create_file<P: AsRef<Path>>(filename: P) -> Fallible<fs::File> {
    return Ok(fs::File::create(filename.as_ref())
        .with_context(|e| format!("Can't open file {:?} for writing: {}", filename.as_ref().display(), e))?);
}

/// Open file for writing, passes a Writer to a closure, and closes the file, with O_TMPFILE
/// semantics.
pub fn write_file<P, F>(
    filename: P,
    f: F
) -> Fallible<()>
where
    P: AsRef<Path>,
    F: Fn(&mut io::BufWriter<&mut fs::File>) -> Fallible<()>,
{
    // XXX: enhance with tempfile + linkat + rename dance
    let mut file = create_file(filename)?;
    {
        let mut w = io::BufWriter::new(&mut file);
        f(&mut w)?;
    }
    file.sync_all()?;
    Ok(())
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
        )
        .unwrap();
        assert_eq!(r, "ostree/fedora/ppc64le/blah/ppc64le/whee");
        let r = varsubst("${osvendor}${basearch}", &subs).unwrap();
        assert_eq!(r, "fedorappc64le");
        let r = varsubst("${osvendor}", &subs).unwrap();
        assert_eq!(r, "fedora");
    }

    #[test]
    fn test_open_file_nonexistent() {
        let path = "/usr/share/empty/manifest.yaml";
        match open_file(path) {
            Err(ref e) => assert!(e
                .to_string()
                .starts_with(format!("Can't open file {:?} for reading:", path).as_str())),
            Ok(_) => panic!("Expected nonexistent treefile error for {}", path),
        }
    }
}

mod ffi {
    use super::*;
    use glib;
    use glib_sys;
    use libc;
    use std::ffi::CString;
    use std::os::unix::io::IntoRawFd;
    use std::ptr;

    use crate::ffiutil::*;

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
