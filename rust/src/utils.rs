//! The generic catchall "utils" file that every
//! project has.

/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use crate::cxxrsutil::*;
use anyhow::{bail, Context, Result};
use std::collections::HashMap;
use std::io::prelude::*;
use std::os::unix::io::IntoRawFd;
use std::path::Path;
use std::{fs, io};

use curl::easy::Easy;

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

/// Given a URL, download it to an O_TMPFILE (temporary file descriptor).
/// This is a thin wrapper for `download_urls_to_tmpfiles`.
pub(crate) fn download_url_to_tmpfile(url: &str, progress: bool) -> Result<fs::File> {
    // Unwrap safety: multiple case returns the same number of values
    Ok(download_urls_to_tmpfiles(vec![url], progress)?
        .into_iter()
        .next()
        .expect("file"))
}

/// Given multiple URLs, download them to an O_TMPFILE (temporary file descriptor).
/// This uses sane defaults for fetching files, such as following the location
/// and making HTTP level errors also return an error rather than the error page HTML.
pub(crate) fn download_urls_to_tmpfiles<S: AsRef<str>>(
    urls: Vec<S>,
    progress: bool,
) -> Result<Vec<fs::File>> {
    let mut handle = Easy::new();
    handle.follow_location(true)?;
    handle.fail_on_error(true)?;
    urls.iter()
        .map(|url| {
            let url = url.as_ref();
            let mut tmpf = tempfile::tempfile()?;
            if progress {
                print!("Downloading {}...", url);
            }
            // Create an internally invoked closure so we can
            // capture the success/error case to complete the progress output line.
            let mut dl = || -> Result<()> {
                let mut output = io::BufWriter::new(&mut tmpf);
                handle.url(url)?;

                let mut transfer = handle.transfer();
                transfer
                    .write_function(|data| output.write_all(data).and(Ok(data.len())).or(Ok(0)))?;
                transfer.perform()?;
                Ok(())
            };
            if progress {
                match dl() {
                    Ok(()) => println!("done"),
                    Err(e) => {
                        println!("failed");
                        return Err(e);
                    }
                }
            } else {
                dl()?;
            }
            tmpf.seek(io::SeekFrom::Start(0))?;
            Ok(tmpf)
        })
        .collect()
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
fn varsubst(instr: &str, vars: &HashMap<String, String>) -> Result<String> {
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
    fn more_varsubsts() -> Result<()> {
        let mut subs = HashMap::new();
        subs.insert("basearch".to_string(), "bacon".to_string());
        subs.insert("v".to_string(), "42".to_string());
        let subs = &subs;

        assert_eq!(varsubst("${basearch}", subs)?, "bacon");
        assert_eq!(varsubst("foo/${basearch}/bar", subs)?, "foo/bacon/bar");
        assert_eq!(varsubst("${basearch}/bar", subs)?, "bacon/bar");
        assert_eq!(varsubst("foo/${basearch}", subs)?, "foo/bacon");
        assert_eq!(
            varsubst("foo/${basearch}/${v}/bar", subs)?,
            "foo/bacon/42/bar"
        );
        assert_eq!(varsubst("${v}", subs)?, "42");

        let subs = HashMap::new();
        assert!(varsubst("${v}", &subs).is_err());
        assert!(varsubst("foo/${v}/bar", &subs).is_err());

        assert!(varsubst("${", &subs).is_err());
        assert!(varsubst("foo/${", &subs).is_err());
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

    // Note this is testing C++ code defined in rpmostree-util.cxx
    #[test]
    fn test_next_version() {
        use crate::ffi::util_next_version;
        fn assert_ver(prefix: &str, v: &str, res: &str) {
            assert_eq!(util_next_version(prefix, "", v).expect("version"), res);
        }
        for v in &["", "xyz", "9", "11"] {
            assert_ver("10", v, "10");
        }

        assert_ver("10", "10", "10.1");
        assert_ver("10.1", "10.1", "10.1.1");
        assert_ver("10", "10.0", "10.1");
        assert_ver("10", "10.1", "10.2");
        assert_ver("10", "10.2", "10.3");
        assert_ver("10", "10.3", "10.4");
        assert_ver("10", "10.1.5", "10.2");
        assert_ver("10.1", "10.1.5", "10.1.6");
        assert_ver("10.1", "10.1.1.5", "10.1.2");
        assert_ver("10", "10001", "10");
        assert_ver("10", "101.1", "10");
        assert_ver("10", "10x.1", "10");
        assert_ver("10.1", "10", "10.1");
        assert_ver("10.1", "10.", "10.1");
        assert_ver("10.1", "10.0", "10.1");
        assert_ver("10.1", "10.2", "10.1");
        assert_ver("10.1", "10.12", "10.1");
        assert_ver("10.1", "10.1x", "10.1");
        assert_ver("10.1", "10.1.x", "10.1.1");
        assert_ver("10.1", "10.1.2x", "10.1.3");

        assert_eq!(util_next_version("10", "-", "10").unwrap(), "10-1");
        assert_eq!(util_next_version("10", "-", "10-1").unwrap(), "10-2");
        assert_eq!(util_next_version("10.1", "-", "10.1-5").unwrap(), "10.1-6");

        fn t(pre: &str, last: &str, final_datefmt: &str) {
            let now = glib::DateTime::new_now_utc();
            let final_version = now.format(final_datefmt).unwrap();
            let ver = util_next_version(pre, "", last).expect("version");
            assert_eq!(ver, final_version);
        }
        // Test date updates
        t("10.<date:%Y%m%d>", "10.20001010", "10.%Y%m%d.0");

        // Test increment reset when date changed.
        t("10.<date:%Y%m%d>", "10.20001010.5", "10.%Y%m%d.0");

        let now = glib::DateTime::new_now_utc();
        // Test increment up when same date.
        let prev_version = now.format("10.%Y%m%d.1").unwrap();
        t("10.<date:%Y%m%d>", prev_version.as_str(), "10.%Y%m%d.2");

        // Test append version number.
        t("10.<date:%Y%m%d>", "", "10.%Y%m%d.0");
        let prev_version = now.format("10.%Y%m%d").unwrap();
        t("10.<date:%Y%m%d>.0", prev_version.as_str(), "10.%Y%m%d.0.0");
        let prev_version = now.format("10.%Y%m%d.0").unwrap();
        t("10.<date:%Y%m%d>.0", prev_version.as_str(), "10.%Y%m%d.0.0");
        let prev_version = now.format("10.%Y%m%d.x").unwrap();
        t("10.<date:%Y%m%d>", prev_version.as_str(), "10.%Y%m%d.1");
        let prev_version = now.format("10.%Y%m%d.2.x").unwrap();
        t("10.<date:%Y%m%d>.2", prev_version.as_str(), "10.%Y%m%d.2.1");
        let prev_version = now.format("10.%Y%m%d.1.2x").unwrap();
        t("10.<date:%Y%m%d>.1", prev_version.as_str(), "10.%Y%m%d.1.3");

        // Test variations to the formatting.
        t("10.<date: %Y%m%d>", "10.20001010", "10. %Y%m%d.0");
        t("10.<date:%Y%m%d>.", "10.20001010.", "10.%Y%m%d..0");
        t("10.<date:%Y%m%d>abc", "10.20001010abc", "10.%Y%m%dabc.0");
        t("10.<date:%Y%m%d >", "10.20001010", "10.%Y%m%d .0");
        t(
            "10.<date:text%Y%m%dhere>",
            "10.20001010",
            "10.text%Y%m%dhere.0",
        );
        t(
            "10.<date:text %Y%m%d here>",
            "10.20001010",
            "10.text %Y%m%d here.0",
        );
        t("10.<date:%Y%m%d here>", "10.20001010", "10.%Y%m%d here.0");

        // Test equal last version and prefix.
        let prev_version = now.format("10.%Y%m%d").unwrap();
        t("10.<date:%Y%m%d>", prev_version.as_str(), "10.%Y%m%d.0");

        // Test different prefix from last version.
        t("10.<date:%Y%m%d>", "10", "10.%Y%m%d.0");

        // Test no field given.
        t("10.<date: >", "10.20001010", "10. .0");
        t("10.<date:>", "10.20001010", "10..0");
        t("10.<wrongtag: >", "10.20001010", "10.<wrongtag: >");

        // Test invalid format
        assert!(util_next_version("10.<date:%E>", "", "10.20001010").is_err());
    }
}

/// TODO: cxx-rs doesn't support maps yet
pub(crate) fn varsubstitute(s: &str, subs: &Vec<crate::ffi::StringMapping>) -> CxxResult<String> {
    let m = subs.iter().cloned().map(|i| (i.k, i.v)).collect();
    Ok(varsubst(s, &m)?)
}

pub(crate) fn get_features() -> Vec<String> {
    let mut r = Vec::new();
    #[cfg(feature = "fedora-integration")]
    r.push("fedora-integration".to_string());
    r
}

fn impl_sealed_memfd(description: &str, content: &[u8]) -> Result<std::fs::File> {
    let mfd = memfd::MemfdOptions::default()
        .allow_sealing(true)
        .close_on_exec(true)
        .create(description)?;
    mfd.as_file().set_len(content.len() as u64)?;
    mfd.as_file().write_all(content)?;
    let mut seals = memfd::SealsHashSet::new();
    seals.insert(memfd::FileSeal::SealShrink);
    seals.insert(memfd::FileSeal::SealGrow);
    seals.insert(memfd::FileSeal::SealWrite);
    seals.insert(memfd::FileSeal::SealSeal);
    mfd.add_seals(&seals)?;
    Ok(mfd.into_file())
}

/// Create a fully sealed "memfd" (memory file descriptor) from an array of bytes.
/// For more information see https://docs.rs/memfd/0.3.0/memfd/ and
/// `man memfd_create`.
pub(crate) fn sealed_memfd(description: &str, content: &[u8]) -> CxxResult<i32> {
    let mfd = impl_sealed_memfd(description, content)?;
    Ok(mfd.into_raw_fd())
}

/// Map Rust architecture constants to the ones used by DNF.
/// Rust architecture constants: https://doc.rust-lang.org/std/env/consts/constant.ARCH.html
/// DNF mapping: https://github.com/rpm-software-management/dnf/blob/4.5.2/dnf/rpm/__init__.py#L88
pub(crate) fn get_rpm_basearch() -> String {
    match std::env::consts::ARCH {
        "arm" => "armhfp".to_string(),
        "powerpc" => "ppc".to_string(),
        "powerpc64" => "ppc64".to_string(),
        "sparc64" => "sparc".to_string(),
        "x86" => "i386".to_string(),
        s => s.to_string(),
    }
}
