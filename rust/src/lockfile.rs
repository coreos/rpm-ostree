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
        let lockfile: LockfileConfig =
            utils::parse_stream(&utils::InputFormat::JSON, &mut input).unwrap();
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
        let mut base_lockfile: LockfileConfig =
            utils::parse_stream(&utils::InputFormat::JSON, &mut base_input).unwrap();
        assert!(base_lockfile.packages.len() == 2);

        let mut override_input = io::BufReader::new(OVERRIDE_JS.as_bytes());
        let override_lockfile: LockfileConfig =
            utils::parse_stream(&utils::InputFormat::JSON, &mut override_input).unwrap();
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

use crate::cxxrsutil::CxxResult;
use crate::ffi::*;
use libdnf_sys::*;

pub(crate) fn ror_lockfile_read(filenames: &Vec<String>) -> CxxResult<Vec<StringMapping>> {
    Ok(lockfile_parse_multiple(&filenames)?
        .packages
        .into_iter()
        .map(|(k, v)| StringMapping {
            k: format!("{}-{}", k, v.evra),
            v: v.digest.unwrap_or_else(|| "".into()),
        })
        .collect())
}

pub(crate) fn ror_lockfile_write(
    filename: &str,
    packages: Vec<u64>,
    rpmmd_repos: Vec<u64>,
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

    for pkg in packages {
        let pkg_ref = unsafe { &mut *(pkg as *mut libdnf_sys::DnfPackage) };
        let name = dnf_package_get_name(pkg_ref).unwrap();
        let evr = dnf_package_get_evr(pkg_ref).unwrap();
        let arch = dnf_package_get_arch(pkg_ref).unwrap();

        let chksum = crate::ffi::get_repodata_chksum_repr(pkg_ref).unwrap();
        lockfile.packages.insert(
            name.as_str().to_string(),
            LockedPackage {
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

    for rpmmd_repo in rpmmd_repos {
        let repo_ref = unsafe { &mut *(rpmmd_repo as *mut libdnf_sys::DnfRepo) };
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
