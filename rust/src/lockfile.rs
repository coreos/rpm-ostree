/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//! Manage lockfiles which are a model to pin specific package
//! versions via JSON files (usually stored in git).

/* Copied and adapted from: treefile.rs
 */

pub use self::ffi::*;
use crate::utils;
use anyhow::Result;
use chrono::prelude::*;
use openat_ext::OpenatDirExt;
use serde_derive::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::convert::TryInto;
use std::io;
use std::iter::Extend;
use std::path::Path;
use std::pin::Pin;

/// Given a lockfile filename, parse it
fn lockfile_parse<P: AsRef<Path>>(filename: P) -> Result<LockfileConfig> {
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
fn lockfile_parse_multiple<P: AsRef<Path>>(filenames: &[P]) -> Result<LockfileConfig> {
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
/// ```json
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
///        "name3": {
///             "evr": "EVR3",
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
#[serde(untagged)]
enum LockedPackage {
    Evr {
        evr: String,
        digest: Option<String>,
    },
    Evra {
        evra: String,
        digest: Option<String>,
    },
}

impl LockedPackage {
    fn evra_glob(&self) -> String {
        match self {
            LockedPackage::Evr { evr, digest: _ } => format!("{}.*", evr),
            LockedPackage::Evra { evra, digest: _ } => evra.into(),
        }
    }
    fn digest(&self) -> String {
        match self {
            LockedPackage::Evr { evr: _, digest } => digest.clone().unwrap_or_default(),
            LockedPackage::Evra { evra: _, digest } => digest.clone().unwrap_or_default(),
        }
    }
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
        },
        "baz": {
            "evr": "2.1.1-1"
        }
    }
}
"###;

    fn assert_evra(locked_package: &LockedPackage, expected_evra: &str) {
        match locked_package {
            LockedPackage::Evra { evra, digest: _ } => assert_eq!(evra, expected_evra),
            _ => panic!("Expected LockedPackage::Evra variant"),
        }
    }

    fn assert_evr(locked_package: &LockedPackage, expected_evr: &str) {
        match locked_package {
            LockedPackage::Evr { evr, digest: _ } => assert_eq!(evr, expected_evr),
            _ => panic!("Expected LockedPackage::Evr variant"),
        }
    }

    #[test]
    fn basic_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE_JS.as_bytes());
        let lockfile: LockfileConfig =
            utils::parse_stream(&utils::InputFormat::JSON, &mut input).unwrap();
        assert!(lockfile.packages.len() == 3);
        assert_evra(lockfile.packages.get("foo").unwrap(), "1.0-1.noarch");
        assert_evra(lockfile.packages.get("bar").unwrap(), "0.8-15.x86_64");
        assert_evr(lockfile.packages.get("baz").unwrap(), "2.1.1-1");
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
        let mut base_lockfile: LockfileConfig =
            utils::parse_stream(&utils::InputFormat::JSON, &mut base_input).unwrap();
        assert!(base_lockfile.packages.len() == 3);

        let mut override_input = io::BufReader::new(OVERRIDE_JS.as_bytes());
        let override_lockfile: LockfileConfig =
            utils::parse_stream(&utils::InputFormat::JSON, &mut override_input).unwrap();
        assert!(override_lockfile.packages.len() == 1);

        base_lockfile.merge(override_lockfile);
        assert!(base_lockfile.packages.len() == 3);
        assert_evra(base_lockfile.packages.get("foo").unwrap(), "2.0-2.noarch");
        assert_evra(base_lockfile.packages.get("bar").unwrap(), "0.8-15.x86_64");
        assert_evr(base_lockfile.packages.get("baz").unwrap(), "2.1.1-1");
    }

    static INVALID_PRELUDE_JS: &str = r###"
{
    "packages": {},
    "unknown-field": "true"
}
"###;

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

use crate::cxxrsutil::CxxResult;
use crate::ffi::*;
use libdnf_sys::*;

pub(crate) fn lockfile_read(filenames: &Vec<String>) -> CxxResult<Vec<StringMapping>> {
    Ok(lockfile_parse_multiple(&filenames)?
        .packages
        .into_iter()
        .map(|(k, v)| StringMapping {
            k: format!("{}-{}", k, v.evra_glob()),
            v: v.digest(),
        })
        .collect())
}

pub(crate) fn lockfile_write(
    filename: &str,
    mut packages: Pin<&mut crate::ffi::CxxGObjectArray>,
    mut rpmmd_repos: Pin<&mut crate::ffi::CxxGObjectArray>,
) -> CxxResult<()> {
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
        }),
    };

    for i in 0..(packages.as_mut().length()) {
        let pkg = packages.as_mut().get(i);
        let pkg_ref = unsafe { &mut *(&mut pkg.0 as *mut _ as *mut libdnf_sys::DnfPackage) };
        let name = dnf_package_get_name(pkg_ref).unwrap();
        let evr = dnf_package_get_evr(pkg_ref).unwrap();
        let arch = dnf_package_get_arch(pkg_ref).unwrap();

        let chksum = crate::ffi::get_repodata_chksum_repr(pkg_ref).unwrap();
        lockfile.packages.insert(
            name.as_str().to_string(),
            LockedPackage::Evra {
                evra: format!("{}.{}", evr.as_str(), arch.as_str()),
                digest: Some(chksum),
            },
        );
    }

    /* just take the ref here to be less verbose */
    let lockfile_repos = lockfile
        .metadata
        .as_mut()
        .unwrap()
        .rpmmd_repos
        .as_mut()
        .unwrap();

    for i in 0..rpmmd_repos.as_mut().length() {
        let repo_ref = rpmmd_repos.as_mut().get(i);
        let repo_ref = unsafe { &mut *(&mut repo_ref.0 as *mut _ as *mut libdnf_sys::DnfRepo) };
        let id = dnf_repo_get_id(repo_ref).unwrap();
        let generated = dnf_repo_get_timestamp_generated(repo_ref).unwrap();
        let generated: i64 = match generated.try_into() {
            Ok(t) => t,
            Err(e) => {
                eprintln!("Invalid rpm-md repo {} timestamp: {}: {}", id, generated, e);
                0
            }
        };
        let generated = match Utc.timestamp_opt(generated, 0) {
            chrono::offset::LocalResult::Single(t) => t,
            _ => {
                eprintln!("Invalid rpm-md repo {} timestamp: {}", id, generated);
                Utc.timestamp(0, 0)
            }
        };
        lockfile_repos.insert(id, LockfileRepoMetadata { generated });
    }

    let filename = Path::new(filename);
    let lockfile_dir = openat::Dir::open(filename.parent().unwrap_or_else(|| Path::new("/")))?;
    let basename = filename.file_name().expect("filename");
    lockfile_dir.write_file_with(basename, 0o644, |w| -> Result<()> {
        Ok(serde_json::to_writer_pretty(w, &lockfile)?)
    })?;
    Ok(())
}
