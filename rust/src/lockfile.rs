/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

/* Copied and adapted from: treefile.rs
 */

use failure::Fallible;
use serde_derive::{Deserialize, Serialize};
use serde_json;
use std::collections::{HashMap, BTreeMap};
use std::iter::Extend;
use std::path::Path;
use std::io;
use chrono::prelude::*;
use std::convert::TryInto;

use crate::utils;

/// Given a lockfile filename, parse it
fn lockfile_parse<P: AsRef<Path>>(filename: P,) -> Fallible<LockfileConfig> {
    let filename = filename.as_ref();
    let fmt = utils::InputFormat::detect_from_filename(filename)?;
    let mut f = io::BufReader::new(utils::open_file(filename)?);
    let lf = utils::parse_stream(&fmt, &mut f).map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("Parsing {}: {}", filename.to_string_lossy(), e.to_string()),
        )
    })?;
    Ok(lf)
}

/// Given lockfile filenames, parse them. Later lockfiles may override packages from earlier ones.
fn lockfile_parse_multiple<P: AsRef<Path>>(filenames: &[P]) -> Fallible<LockfileConfig> {
    let mut final_lockfile: Option<LockfileConfig> = None;
    for filename in filenames {
        let lf = lockfile_parse(filename)?;
        if let Some(ref mut final_lockfile) = final_lockfile {
            final_lockfile.merge(lf);
        } else {
            final_lockfile = Some(lf);
        }
    }
    Ok(final_lockfile.expect("lockfile_parse: at least one lockfile"))
}

/// Lockfile format:
///
/// ```
/// {
///    "metatada": {
///        "generated": "<rfc3339-timestamp>",
///        "rpmmd_repos": {
///             "repo1": {
///                 "generated": "<rfc3339-timestamp>"
///             },
///             "repo2": {
///                 "generated": "<rfc3339-timestamp>"
///             },
///             ...
///        }
///    }
///    "packages": {
///        "name1": {
///             "evra": "EVRA1",
///             "digest": "<digest-algo>:<digest>"
///        },
///        "name2": {
///             "evra": "EVRA2",
///             "digest": "<digest-algo>:<digest>"
///        },
///        ...
///    }
/// }
/// ```
///
/// XXX: One known limitation of this format right now is that it's not compatible with multilib.
/// TBD whether we care about this.
///
#[derive(Serialize, Deserialize, Debug)]
#[serde(deny_unknown_fields)]
struct LockfileConfig {
    packages: BTreeMap<String, LockedPackage>,
    metadata: Option<LockfileConfigMetadata>,
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(deny_unknown_fields)]
struct LockfileConfigMetadata {
    generated: Option<DateTime<Utc>>,
    rpmmd_repos: Option<BTreeMap<String, LockfileRepoMetadata>>,
}

#[derive(Serialize, Deserialize, Debug)]
struct LockfileRepoMetadata {
    generated: DateTime<Utc>,
}

#[derive(Serialize, Deserialize, Debug)]
struct LockedPackage {
    evra: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    digest: Option<String>,
}

impl LockfileConfig {
    fn merge(&mut self, other: LockfileConfig) {
        self.packages.extend(other.packages);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    static VALID_PRELUDE_JS: &str = r###"
{
    "packages": {
        "foo": {
            "evra": "1.0-1.noarch",
            "digest": "sha256:deadcafe"
        },
        "bar": {
            "evra": "0.8-15.x86_64",
            "digest": "sha256:cafedead"
        }
    }
}
"###;

    static INVALID_PRELUDE_JS: &str = r###"
{
    "packages": {},
    "unknown-field": "true"
}
"###;

    #[test]
    fn basic_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE_JS.as_bytes());
        let lockfile: LockfileConfig = utils::parse_stream(&utils::InputFormat::JSON, &mut input).unwrap();
        assert!(lockfile.packages.len() == 2);
    }

    static OVERRIDE_JS: &str = r###"
{
    "packages": {
        "foo": {
            "evra": "2.0-2.noarch",
            "digest": "sha256:dada"
        }
    }
}
"###;

    #[test]
    fn basic_valid_override() {
        let mut base_input = io::BufReader::new(VALID_PRELUDE_JS.as_bytes());
        let mut base_lockfile: LockfileConfig = utils::parse_stream(&utils::InputFormat::JSON, &mut base_input).unwrap();
        assert!(base_lockfile.packages.len() == 2);

        let mut override_input = io::BufReader::new(OVERRIDE_JS.as_bytes());
        let override_lockfile: LockfileConfig = utils::parse_stream(&utils::InputFormat::JSON, &mut override_input).unwrap();
        assert!(override_lockfile.packages.len() == 1);

        base_lockfile.merge(override_lockfile);
        assert!(base_lockfile.packages.len() == 2);
        assert!(base_lockfile.packages.get("foo").unwrap().evra == "2.0-2.noarch");
        assert!(base_lockfile.packages.get("bar").unwrap().evra == "0.8-15.x86_64");
    }

    #[test]
    fn test_invalid() {
        let mut input = io::BufReader::new(INVALID_PRELUDE_JS.as_bytes());
        match utils::parse_stream::<LockfileConfig, _>(&utils::InputFormat::JSON, &mut input) {
            Err(ref e) => match e.downcast_ref::<io::Error>() {
                Some(ref ioe) if ioe.kind() == io::ErrorKind::InvalidInput => {}
                _ => panic!("Expected invalid lockfile, not {}", e.to_string()),
            },
            Ok(_) => panic!("Expected invalid lockfile"),
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
    use crate::libdnf_sys::*;

    #[no_mangle]
    pub extern "C" fn ror_lockfile_read(
        filenames: *mut *mut libc::c_char,
        gerror: *mut *mut glib_sys::GError,
    ) -> *mut glib_sys::GHashTable {
        let filenames = ffi_strv_to_os_str_vec(filenames);
        match lockfile_parse_multiple(&filenames) {
            Err(ref e) => {
                error_to_glib(e, gerror);
                ptr::null_mut()
            },
            Ok(lockfile) => {
                // would be more efficient to just create a GHashTable manually here, but eh...
                let map = lockfile.packages
                    .into_iter()
                    .fold(HashMap::<String, String>::new(), |mut acc, (k, v)| {
                        acc.insert(format!("{}-{}", k, v.evra), v.digest.unwrap_or("".into()));
                        acc
                    }
                );
                map.to_glib_full()
            }
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_lockfile_write(
        filename: *const libc::c_char,
        packages: *mut glib_sys::GPtrArray,
        rpmmd_repos: *mut glib_sys::GPtrArray,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let filename = ffi_view_os_str(filename);
        let packages: Vec<*mut DnfPackage> = ffi_ptr_array_to_vec(packages);
        let rpmmd_repos: Vec<*mut DnfRepo> = ffi_ptr_array_to_vec(rpmmd_repos);

        // get current time, but scrub nanoseconds; it's overkill to serialize that
        let now = {
            let t = Utc::now();
            Utc::today().and_hms_nano(t.hour(), t.minute(), t.second(), 0)
        };

        let mut lockfile = LockfileConfig {
            packages: BTreeMap::new(),
            metadata: Some(LockfileConfigMetadata {
                generated: Some(now),
                rpmmd_repos: Some(BTreeMap::new()),
            })
        };

        for pkg in packages {
            let name = ffi_new_string(unsafe { dnf_package_get_name(pkg) });
            let evr = ffi_view_str(unsafe { dnf_package_get_evr(pkg) });
            let arch = ffi_view_str(unsafe { dnf_package_get_arch(pkg) });

            let mut chksum: *mut libc::c_char = ptr::null_mut();
            let r = unsafe { rpmostree_get_repodata_chksum_repr(pkg, &mut chksum, gerror) };
            if r == 0 {
                return r;
            }

            lockfile.packages.insert(name, LockedPackage {
                evra: format!("{}.{}", evr, arch),
                digest: Some(ffi_new_string(chksum)),
            });

            // forgive me for this sin... need to oxidize chksum_repr()
            unsafe { glib_sys::g_free(chksum as *mut libc::c_void) };
        }

        /* just take the ref here to be less verbose */
        let lockfile_repos = lockfile.metadata.as_mut().unwrap().rpmmd_repos.as_mut().unwrap();

        for rpmmd_repo in rpmmd_repos {
            let id = ffi_new_string(unsafe { dnf_repo_get_id(rpmmd_repo) });
            let generated = unsafe { dnf_repo_get_timestamp_generated(rpmmd_repo) };
            lockfile_repos.insert(id, LockfileRepoMetadata {
                generated: Utc.timestamp(generated.try_into().unwrap(), 0),
            });
        }

        int_glib_error(utils::write_file(filename, |w| {
            serde_json::to_writer_pretty(w, &lockfile).map_err(failure::Error::from)
        }), gerror)
    }
}
pub use self::ffi::*;
