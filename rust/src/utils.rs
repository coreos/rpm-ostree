/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use anyhow::{bail, Context, Result};
use std::collections::HashMap;
use std::io::prelude::*;
use std::path::Path;
use std::{fs, io};
use tempfile;

use curl::easy::Easy;

use serde_json;
use serde_yaml;

#[derive(PartialEq)]
/// Supported config serialization used by treefile and lockfile
pub enum InputFormat {
    YAML,
    JSON,
}

impl InputFormat {
    pub fn detect_from_filename<P: AsRef<Path>>(filename: P) -> Result<Self> {
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

/// Given a lockfile/treefile config definition, parse it
pub fn parse_stream<T, R: io::Read>(fmt: &InputFormat, input: &mut R) -> Result<T>
where
    T: serde::de::DeserializeOwned,
{
    let parsed: T = match fmt {
        InputFormat::JSON => {
            let pf: T = serde_json::from_reader(input).map_err(|e| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("serde-json: {}", e.to_string()),
                )
            })?;
            pf
        }
        InputFormat::YAML => {
            let pf: T = serde_yaml::from_reader(input).map_err(|e| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("serde-yaml: {}", e.to_string()),
                )
            })?;
            pf
        }
    };
    Ok(parsed)
}

fn download_url_to_tmpfile(url: &str) -> Result<fs::File> {
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
pub fn open_file<P: AsRef<Path>>(filename: P) -> Result<fs::File> {
    Ok(fs::File::open(filename.as_ref()).with_context(|| {
        format!(
            "Can't open file {:?} for reading",
            filename.as_ref().display()
        )
    })?)
}

/// Open file for writing and provide context containing filename on failures.
pub fn create_file<P: AsRef<Path>>(filename: P) -> Result<fs::File> {
    Ok(fs::File::create(filename.as_ref()).with_context(|| {
        format!(
            "Can't open file {:?} for writing",
            filename.as_ref().display()
        )
    })?)
}

// Surprising we need a wrapper for this... parent() returns a slice of its buffer, so doesn't
// handle going up relative paths well: https://github.com/rust-lang/rust/issues/36861
pub fn parent_dir(filename: &Path) -> Option<&Path> {
    filename
        .parent()
        .map(|p| if p.as_os_str() == "" { ".".as_ref() } else { p })
}

pub fn decompose_sha256_nevra(v: &str) -> Result<(&str, &str)> {
    let parts: Vec<&str> = v.splitn(2, ':').collect();
    match (parts.get(0), parts.get(1)) {
        (Some(_), None) => bail!("Missing : in {}", v),
        (Some(first), Some(rest)) => {
            ostree::validate_checksum_string(rest)?;
            Ok((first, rest))
        }
        (_, _) => unreachable!(),
    }
}

/// Given an input string `s`, replace variables of the form `${foo}` with
/// values provided in `vars`.  No quoting syntax is available, so it is
/// not possible to have a literal `${` in the string.
pub fn varsubst(instr: &str, vars: &HashMap<String, String>) -> Result<String> {
    let mut buf = instr;
    let mut s = "".to_string();
    while !buf.is_empty() {
        if let Some(start) = buf.find("${") {
            let (prefix, rest) = buf.split_at(start);
            let rest = &rest[2..];
            s.push_str(prefix);
            if let Some(end) = rest.find('}') {
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
    Ok(s)
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
    fn test_decompose_sha256_nevra() -> Result<()> {
        assert!(decompose_sha256_nevra("").is_err());
        assert!(decompose_sha256_nevra("foo:bar").is_err());
        // Has a Q in the middle
        assert!(decompose_sha256_nevra(
            "foo:41af286dc0b172ed2f1ca934fd2278de4a119Q302ffa07087cea2682e7d372e3"
        )
        .is_err());
        assert!(decompose_sha256_nevra("foo:bar:baz").is_err());
        let c = "41af286dc0b172ed2f1ca934fd2278de4a1199302ffa07087cea2682e7d372e3";
        let foo = format!("foo:{}", c);
        let (n, p) = decompose_sha256_nevra(&foo).context("testing foo")?;
        assert_eq!(n, "foo");
        assert_eq!(p, c);
        Ok(())
    }

    #[test]
    fn test_open_file_nonexistent() {
        let path = "/usr/share/empty/manifest.yaml";
        match open_file(path) {
            Err(ref e) => assert!(e
                .to_string()
                .starts_with(format!("Can't open file {:?} for reading", path).as_str())),
            Ok(_) => panic!("Expected nonexistent treefile error for {}", path),
        }
    }
}

mod ffi {
    use super::*;
    use crate::ffiutil::*;
    use glib;
    use glib::translate::*;
    use glib_sys;
    use libc;
    use std::ffi::CString;
    use std::os::unix::io::IntoRawFd;
    use std::ptr;

    pub(crate) fn download_to_fd(url: &str) -> Result<i32> {
        download_url_to_tmpfile(url).map(|f| f.into_raw_fd())
    }

    #[no_mangle]
    pub extern "C" fn ror_util_varsubst(
        s: *const libc::c_char,
        h: *mut glib_sys::GHashTable,
        gerror: *mut *mut glib_sys::GError,
    ) -> *mut libc::c_char {
        let s: Borrowed<glib::GString> = unsafe { from_glib_borrow(s) };
        let h_rs: HashMap<String, String> =
            unsafe { glib::translate::FromGlibPtrContainer::from_glib_none(h) };
        match varsubst(s.as_str(), &h_rs) {
            Ok(s) => CString::new(s).unwrap().into_raw(),
            Err(e) => {
                error_to_glib(&e, gerror);
                ptr::null_mut()
            }
        }
    }
}
pub use self::ffi::*;
