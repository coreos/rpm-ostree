//! The generic catchall "utils" file that every
//! project has.

/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use crate::cxxrsutil::*;
use crate::variant_utils;
use anyhow::{bail, Context, Result};
use cap_std_ext::cap_std;
use glib::translate::ToGlibPtr;
use glib::Variant;
use once_cell::sync::Lazy;
use ostree_ext::prelude::*;
use ostree_ext::{glib, ostree};
use regex::Regex;
use std::borrow::Cow;
use std::collections::{HashMap, HashSet};
use std::io::prelude::*;
use std::os::unix::io::{AsRawFd, IntoRawFd};
use std::path::Path;
use std::{fs, io};

#[derive(Debug, PartialEq)]
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
                io::Error::new(io::ErrorKind::InvalidInput, format!("serde-json: {e}"))
            })?;
            pf
        }
        InputFormat::YAML => {
            let pf: T = serde_yaml::from_reader(input).map_err(|e| {
                io::Error::new(io::ErrorKind::InvalidInput, format!("serde-yaml: {e}"))
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
    let handle = reqwest::blocking::Client::new();
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
                let req = handle.get(url).build()?;
                handle
                    .execute(req)?
                    .error_for_status()?
                    .copy_to(&mut output)?;
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
    fs::File::open(filename.as_ref()).with_context(|| {
        format!(
            "Can't open file {:?} for reading",
            filename.as_ref().display()
        )
    })
}

/// Open file for writing and provide context containing filename on failures.
pub fn create_file<P: AsRef<Path>>(filename: P) -> Result<fs::File> {
    fs::File::create(filename.as_ref()).with_context(|| {
        format!(
            "Can't open file {:?} for writing",
            filename.as_ref().display()
        )
    })
}

// Surprising we need a wrapper for this... parent() returns a slice of its buffer, so doesn't
// handle going up relative paths well: https://github.com/rust-lang/rust/issues/36861
pub fn parent_dir(filename: &Path) -> Option<&Path> {
    filename
        .parent()
        .map(|p| if p.as_os_str() == "" { ".".as_ref() } else { p })
}

/// Call a faillible future, while monitoring `cancellable` and return an error if cancelled.
pub(crate) async fn run_with_cancellable<F, R>(
    f: F,
    cancellable: &ostree_ext::gio::Cancellable,
) -> Result<R>
where
    F: futures::Future<Output = Result<R>>,
{
    // Bridge GCancellable to a tokio notification
    let notify = std::sync::Arc::new(tokio::sync::Notify::new());
    let notify2 = notify.clone();
    cancellable.connect_cancelled(move |_| notify2.notify_one());
    cancellable.set_error_if_cancelled()?;
    tokio::select! {
       r = f => r,
       _ = notify.notified() => {
           Err(anyhow::anyhow!("Operation was cancelled"))
       }
    }
}

/// Parse the 2-tuple `<sha256>:<nevra>` string into a tuple of (nevra, sha256).
/// Note the order is inverted, as it's expected to use the nevra as a key in
/// a mapping.
pub fn decompose_sha256_nevra(v: &str) -> Result<(&str, &str)> {
    let parts: Vec<&str> = v.splitn(2, ':').collect();
    match (parts.get(0), parts.get(1)) {
        (Some(_), None) => bail!("Missing : in {}", v),
        (Some(sha256), Some(nevra)) => {
            ostree::validate_checksum_string(sha256)?;
            Ok((nevra, sha256))
        }
        (_, _) => unreachable!(),
    }
}

/// Given an input string `s`, replace variables of the form `${foo}` with
/// values provided in `vars`.  No quoting syntax is available, so it is
/// not possible to have a literal `${` in the string.
fn varsubst(instr: &str, vars: &HashMap<String, String>) -> Result<String> {
    let mut buf = instr;
    let mut s = String::with_capacity(instr.len());
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

static RUNNING_IN_SYSTEMD: Lazy<bool> = Lazy::new(|| {
    // See https://www.freedesktop.org/software/systemd/man/systemd.exec.html#%24INVOCATION_ID
    std::env::var_os("INVOCATION_ID")
        .filter(|s| !s.is_empty())
        .is_some()
});

/// Checks if the current process is (apparently at least)
/// running under systemd.  We use this in various places
/// to e.g. log to the journal instead of printing to stdout.
pub(crate) fn running_in_systemd() -> bool {
    *RUNNING_IN_SYSTEMD
}

pub fn maybe_shell_quote(input: &str) -> String {
    shellsafe_quote(Cow::Borrowed(input)).into_owned()
}

pub(crate) fn shellsafe_quote(input: Cow<str>) -> Cow<str> {
    static SHELLSAFE: Lazy<Regex> = Lazy::new(|| Regex::new("^[[:alnum:]-._/=:]+$").unwrap());

    if SHELLSAFE.is_match(&input) {
        input
    } else {
        let quoted = glib::shell_quote(input.as_ref());
        // SAFETY: if input is UTF-8, so is the quoted string.
        Cow::Owned(quoted.into_string().expect("non UTF-8 quoted output"))
    }
}

/// Create a new cap_std directory for an openat version.
/// This creates a new file descriptor, because we can't guarantee they are
/// interchangable; for example right now openat uses `O_PATH`
#[allow(dead_code)]
pub(crate) fn openat_to_dirfd(f: &openat::Dir) -> Result<cap_std::fs::Dir> {
    // For now we just delegate to the FFI code
    let r = unsafe { crate::ffiutil::ffi_dirfd(f.as_raw_fd())? };
    Ok(r)
}

/// Translate a relative unprefixed path according to ostree rules, if needed.
///
/// This returns an ostree-compatible path, or an empty string if no translation
/// was performed.
pub fn translate_path_for_ostree(path: &str) -> String {
    translate_path_for_ostree_impl(path).unwrap_or_default()
}

/// Translate a relative unprefixed path according to ostree rules, if needed.
pub(crate) fn translate_path_for_ostree_impl(path: &str) -> Option<String> {
    assert!(!path.starts_with('/'));
    assert!(!path.starts_with("./"));

    // etc/foo -> usr/etc/foo
    if path == "etc" || path.starts_with("etc/") {
        return Some("usr/".to_string() + path);
    }

    // boot/foo -> usr/lib/ostree-boot/foo
    if let Some(prefixless) = path.strip_prefix("boot/") {
        return Some("usr/lib/ostree-boot/".to_string() + prefixless);
    }

    /* Special hack for https://bugzilla.redhat.com/show_bug.cgi?id=1290659
     * See also commit 4a86bdd19665700fa308461510c9decd63e31a03
     * and rpmostree_postprocess_selinux_policy_store_location().
     */
    if let Some(prefixless) = path.strip_prefix("var/lib/selinux/targeted/") {
        return Some("usr/etc/selinux/targeted/".to_string() + prefixless);
    }

    // opt/foo -> usr/lib/opt/foo
    if path == "opt" || path.starts_with("opt/") {
        return Some("usr/lib/".to_string() + path);
    }

    // var/lib/{special}/foo -> usr/lib/{special}/foo
    if let Some(prefixless) = path.strip_prefix("var/lib/") {
        let varlib_cases = &["alternatives", "vagrant"];
        for entry in varlib_cases {
            if prefixless.starts_with(entry) {
                return Some("usr/lib/".to_string() + prefixless);
            }
        }
    }

    // All remaining cases do not need translation.
    None
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
        let foo = format!("{}:foo", c);
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
            let now = glib::DateTime::new_now_utc().unwrap();
            let final_version = now.format(final_datefmt).unwrap();
            let ver = util_next_version(pre, "", last).expect("version");
            assert_eq!(ver, final_version);
        }
        // Test date updates
        t("10.<date:%Y%m%d>", "10.20001010", "10.%Y%m%d.0");

        // Test increment reset when date changed.
        t("10.<date:%Y%m%d>", "10.20001010.5", "10.%Y%m%d.0");

        let now = glib::DateTime::new_now_utc().unwrap();
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

    #[test]
    fn test_translate_path_for_ostree() {
        let cases = [
            ("usr/lib/foo", None),
            ("var/lib/foo", None),
            ("etc", Some("usr/etc")),
            ("etc/foo", Some("usr/etc/foo")),
            ("boot/foo", Some("usr/lib/ostree-boot/foo")),
            (
                "var/lib/selinux/targeted/foo",
                Some("usr/etc/selinux/targeted/foo"),
            ),
            ("opt", Some("usr/lib/opt")),
            ("opt/foo", Some("usr/lib/opt/foo")),
            ("var/lib/alternatives/foo", Some("usr/lib/alternatives/foo")),
            ("var/lib/vagrant/foo", Some("usr/lib/vagrant/foo")),
        ];

        for (input, expected) in cases {
            let translated = translate_path_for_ostree_impl(input);
            assert_eq!(
                translated,
                expected.map(|v| v.to_string()),
                "Input: {input}"
            );
        }
    }
}

/// TODO: cxx-rs doesn't support maps yet
pub(crate) fn varsubstitute(s: &str, subs: &Vec<crate::ffi::StringMapping>) -> CxxResult<String> {
    let m = subs.iter().cloned().map(|i| (i.k, i.v)).collect();
    Ok(varsubst(s, &m)?)
}

pub(crate) fn get_features() -> Vec<String> {
    // These constant features were originally set in configure.ac, but have migrated to
    // Rust in the interest in having less logic in autoconf.
    let defaults = ["rust", "compose", "container"].into_iter().map(Some);
    let conditionals = [
        cfg!(feature = "fedora-integration").then(|| "fedora-integration"),
        cfg!(feature = "bin-unit-tests").then(|| "bin-unit-tests"),
        cfg!(feature = "rhsm").then(|| "rhsm"),
    ]
    .into_iter();
    defaults
        .chain(conditionals)
        .flatten()
        .map(String::from)
        .collect()
}

pub(crate) fn impl_sealed_memfd(description: &str, content: &[u8]) -> Result<std::fs::File> {
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
///
/// Rust architecture constants: https://doc.rust-lang.org/std/env/consts/constant.ARCH.html
/// DNF mapping: https://github.com/rpm-software-management/dnf/blob/4.5.2/dnf/rpm/__init__.py#L88
pub fn get_rpm_basearch() -> String {
    match std::env::consts::ARCH {
        "arm" => "armhfp".to_string(),
        "powerpc" => "ppc".to_string(),
        "powerpc64" if cfg!(target_endian = "big") => "ppc64".to_string(),
        "powerpc64" if cfg!(target_endian = "little") => "ppc64le".to_string(),
        "sparc64" => "sparc".to_string(),
        "x86" => "i386".to_string(),
        s => s.to_string(),
    }
}

// oddly, AFAICT there isn't an easy way to use GV_ADVISORIES_TYPE_STR in this definition
const GV_ADVISORIES_TYPE_STR: &str = "a(suuasa{sv})";

static GV_ADVISORIES_TYPE: Lazy<&'static glib::VariantTy> =
    Lazy::new(|| glib::VariantTy::new(GV_ADVISORIES_TYPE_STR).unwrap());

fn get_commit_advisories(repo: &ostree::Repo, checksum: &str) -> Result<Option<glib::Variant>> {
    let (commit, _) = repo.load_commit(checksum)?;
    let metadata = &commit.child_value(0);
    let metadata = variant_utils::byteswap_be_to_native(metadata);
    let dict = &glib::VariantDict::new(Some(&metadata));
    Ok(dict.lookup_value("rpmostree.advisories", Some(&GV_ADVISORIES_TYPE)))
}

/// Compares advisories between two checksums and returns new ones in checksum_to ("diff" is a bit
/// misleading here; we don't actually say which advisories were removed because normally we only
/// really care about *new* advisories).
pub(crate) fn calculate_advisories_diff(
    repo: &crate::FFIOstreeRepo,
    checksum_from: &str,
    checksum_to: &str,
) -> Result<*mut crate::FFIGVariant> {
    let repo = repo.glib_reborrow();
    let mut new_advisories: Vec<glib::Variant> = Vec::new();
    if let Some(ref advisories_to) = get_commit_advisories(&repo, checksum_to)? {
        let mut previous_advisories: HashSet<String> = HashSet::new();
        if let Some(ref advisories_from) = get_commit_advisories(&repo, checksum_from)? {
            for child in advisories_from.iter() {
                let advisory_id = child.child_value(0).str().unwrap().to_string();
                previous_advisories.insert(advisory_id);
            }
        }

        for child in advisories_to.iter() {
            if !previous_advisories.contains(child.child_value(0).str().unwrap()) {
                new_advisories.push(child);
            }
        }
    }

    let r = Variant::from_array::<(&str, u32, u32, &[&str], HashMap<&str, glib::Variant>)>(
        &new_advisories,
    );
    Ok(r.to_glib_full() as *mut _)
}

pub(crate) fn print_treepkg_diff(sysroot: &str) {
    unsafe {
        crate::ffi::print_treepkg_diff_from_sysroot_path(
            sysroot,
            crate::ffi::RpmOstreeDiffPrintFormat::RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE,
            0,
            std::ptr::null_mut(),
        );
    }
}

/// Implementation of https://github.com/rust-lang/rust/issues/82901 until it's stable.
pub(crate) trait OptionExtGetOrInsertDefault<T> {
    fn ext_get_or_insert_default(&mut self) -> &mut T;
}

impl<T: Default> OptionExtGetOrInsertDefault<T> for Option<T> {
    fn ext_get_or_insert_default(&mut self) -> &mut T {
        match self {
            Some(ref mut t) => t,
            None => self.insert(Default::default()),
        }
    }
}
