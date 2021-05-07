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
use anyhow::{anyhow, bail, Result};
use chrono::prelude::*;
use openat_ext::OpenatDirExt;
use serde_derive::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::convert::TryInto;
use std::io;
use std::iter::Extend;
use std::path::Path;
use std::pin::Pin;

use crate::cxxrsutil::CxxResult;
use libdnf_sys::*;

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
        if lf.packages.is_none() && lf.source_packages.is_none() {
            bail!(
                "Neither 'packages' nor 'source-packages' found in {}",
                filename.as_ref().display()
            );
        }
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
///    },
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
///    },
///    "source-packages": {
///        "name4": "EVR"
///    }
/// }
/// ```
///
/// XXX: One known limitation of this format right now is that it's not compatible with multilib.
/// TBD whether we care about this.
///
#[derive(Serialize, Deserialize, Debug)]
#[serde(deny_unknown_fields)]
#[serde(rename_all = "kebab-case")]
pub(crate) struct LockfileConfig {
    packages: Option<BTreeMap<String, LockedPackage>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    source_packages: Option<BTreeMap<String, String>>,
    metadata: Option<LockfileConfigMetadata>,
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(deny_unknown_fields)]
struct LockfileConfigMetadata {
    generated: Option<DateTime<Utc>>,
    rpmmd_repos: Option<BTreeMap<String, LockfileRepoMetadata>>,
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(deny_unknown_fields)]
struct LockfileRepoMetadata {
    generated: DateTime<Utc>,
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(untagged, deny_unknown_fields)]
enum LockedPackage {
    Evr {
        evr: String,
        digest: Option<String>,
        #[serde(skip_serializing)]
        metadata: Option<BTreeMap<String, serde_json::Value>>,
    },
    Evra {
        evra: String,
        digest: Option<String>,
        #[serde(skip_serializing)]
        metadata: Option<BTreeMap<String, serde_json::Value>>,
    },
}

// NOTE: this is not exactly the same as treefile's merge_map_field
fn merge_map_field<T>(
    base: &mut Option<BTreeMap<String, T>>,
    layer: &mut Option<BTreeMap<String, T>>,
) {
    if let Some(layerv) = layer.take() {
        if let Some(ref mut basev) = base {
            basev.extend(layerv);
        } else {
            *base = Some(layerv);
        }
    }
}

impl LockfileConfig {
    fn merge(&mut self, mut other: LockfileConfig) {
        merge_map_field(&mut self.packages, &mut other.packages);
        merge_map_field(&mut self.source_packages, &mut other.source_packages);
    }

    pub(crate) fn get_locked_packages(&self) -> CxxResult<Vec<crate::ffi::LockedPackage>> {
        self.packages
            .iter()
            .flatten()
            .map(|(k, v)| match v {
                LockedPackage::Evr { evr, digest, .. } => Ok(crate::ffi::LockedPackage {
                    name: k.clone(),
                    evr: evr.clone(),
                    arch: "".into(),
                    digest: digest.clone().unwrap_or_default(),
                }),
                LockedPackage::Evra { evra, digest, .. } => {
                    let evr_arch: Vec<&str> = evra.rsplitn(2, '.').collect();
                    if evr_arch.len() != 2 {
                        Err(anyhow!("package {} has malformed evra: {}", k, evra).into())
                    } else {
                        Ok(crate::ffi::LockedPackage {
                            name: k.clone(),
                            evr: evr_arch[1].into(),
                            arch: evr_arch[0].into(),
                            digest: digest.clone().unwrap_or_default(),
                        })
                    }
                }
            })
            .collect()
    }

    pub(crate) fn get_locked_src_packages(&self) -> CxxResult<Vec<crate::ffi::LockedPackage>> {
        Ok(self
            .source_packages
            .iter()
            .flatten()
            .map(|(k, v)| crate::ffi::LockedPackage {
                name: k.clone(),
                evr: v.clone(),
                arch: "src".into(),
                digest: "".into(),
            })
            .collect())
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
            "digest": "sha256:cafedead",
            "metadata": {
                "foo": true,
                "bar": "baz"
            }
        },
        "baz": {
            "evr": "2.1.1-1",
            "metadata": {}
        }
    },
    "source-packages": {
        "boo": "3.2.1-5",
        "bam": "1.2.3-4"
    }
}
"###;

    fn assert_evra(locked_package: &LockedPackage, expected_evra: &str) {
        match locked_package {
            LockedPackage::Evra { evra, .. } => assert_eq!(evra, expected_evra),
            _ => panic!("Expected LockedPackage::Evra variant"),
        }
    }

    fn assert_evr(locked_package: &LockedPackage, expected_evr: &str) {
        match locked_package {
            LockedPackage::Evr { evr, .. } => assert_eq!(evr, expected_evr),
            _ => panic!("Expected LockedPackage::Evr variant"),
        }
    }

    fn assert_entry<'a, T>(map: &'a Option<BTreeMap<String, T>>, k: &str) -> &'a T {
        map.as_ref().unwrap().get(k).unwrap()
    }

    #[test]
    fn basic_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE_JS.as_bytes());
        let lockfile: LockfileConfig =
            utils::parse_stream(&utils::InputFormat::JSON, &mut input).unwrap();
        assert!(lockfile.packages.is_some());
        assert_eq!(lockfile.packages.as_ref().unwrap().len(), 3);
        assert_evra(assert_entry(&lockfile.packages, "foo"), "1.0-1.noarch");
        assert_evra(assert_entry(&lockfile.packages, "bar"), "0.8-15.x86_64");
        assert_evr(assert_entry(&lockfile.packages, "baz"), "2.1.1-1");
        assert!(lockfile.source_packages.is_some());
        assert_eq!(lockfile.source_packages.as_ref().unwrap().len(), 2);
        assert_eq!(assert_entry(&lockfile.source_packages, "boo"), "3.2.1-5");
        assert_eq!(assert_entry(&lockfile.source_packages, "bam"), "1.2.3-4");
    }

    static OVERRIDE_JS: &str = r###"
{
    "packages": {
        "foo": {
            "evra": "2.0-2.noarch",
            "digest": "sha256:dada"
        }
    },
    "source-packages": {
        "bam": "5.6.7-8"
    }
}
"###;

    #[test]
    fn basic_valid_override() {
        let mut base_input = io::BufReader::new(VALID_PRELUDE_JS.as_bytes());
        let mut base_lockfile: LockfileConfig =
            utils::parse_stream(&utils::InputFormat::JSON, &mut base_input).unwrap();
        assert!(base_lockfile.packages.is_some());
        assert_eq!(base_lockfile.packages.as_ref().unwrap().len(), 3);
        assert!(base_lockfile.source_packages.is_some());
        assert_eq!(base_lockfile.source_packages.as_ref().unwrap().len(), 2);

        let mut override_input = io::BufReader::new(OVERRIDE_JS.as_bytes());
        let override_lockfile: LockfileConfig =
            utils::parse_stream(&utils::InputFormat::JSON, &mut override_input).unwrap();
        assert!(base_lockfile.packages.is_some());
        assert_eq!(override_lockfile.packages.as_ref().unwrap().len(), 1);
        assert!(base_lockfile.source_packages.is_some());
        assert_eq!(override_lockfile.source_packages.as_ref().unwrap().len(), 1);

        base_lockfile.merge(override_lockfile);
        assert!(base_lockfile.packages.is_some());
        assert!(base_lockfile.packages.as_ref().unwrap().len() == 3);
        assert!(base_lockfile.source_packages.is_some());
        assert!(base_lockfile.source_packages.as_ref().unwrap().len() == 2);
        assert_evra(assert_entry(&base_lockfile.packages, "foo"), "2.0-2.noarch");
        assert_evra(
            assert_entry(&base_lockfile.packages, "bar"),
            "0.8-15.x86_64",
        );
        assert_evr(assert_entry(&base_lockfile.packages, "baz"), "2.1.1-1");
        assert_eq!(
            assert_entry(&base_lockfile.source_packages, "boo"),
            "3.2.1-5"
        );
        assert_eq!(
            assert_entry(&base_lockfile.source_packages, "bam"),
            "5.6.7-8"
        );
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

pub(crate) fn lockfile_read(filenames: &Vec<String>) -> CxxResult<Box<LockfileConfig>> {
    Ok(Box::new(lockfile_parse_multiple(&filenames)?))
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
        packages: Some(BTreeMap::new()),
        source_packages: Option::None,
        metadata: Some(LockfileConfigMetadata {
            generated: Some(now),
            rpmmd_repos: Some(BTreeMap::new()),
        }),
    };
    let output_pkgs = lockfile.packages.as_mut().unwrap();

    for i in 0..(packages.as_mut().length()) {
        let pkg = packages.as_mut().get(i);
        let pkg_ref = unsafe { &mut *(&mut pkg.0 as *mut _ as *mut libdnf_sys::DnfPackage) };
        let name = dnf_package_get_name(pkg_ref).unwrap();
        let evr = dnf_package_get_evr(pkg_ref).unwrap();
        let arch = dnf_package_get_arch(pkg_ref).unwrap();

        let chksum = crate::ffi::get_repodata_chksum_repr(pkg_ref).unwrap();
        output_pkgs.insert(
            name.as_str().to_string(),
            LockedPackage::Evra {
                evra: format!("{}.{}", evr.as_str(), arch.as_str()),
                digest: Some(chksum),
                metadata: None,
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
