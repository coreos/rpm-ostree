//! An "origin" declares how we generated an OSTree commit.

/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use crate::cxxrsutil::*;
use crate::ffi::{RefspecType, StringMapping};
use anyhow::{anyhow, bail, Result};
use glib::translate::*;
use glib::KeyFile;
use std::{pin::Pin, result::Result as StdResult};

use std::collections::{BTreeMap, BTreeSet};

const ROJIG_PREFIX: &str = "rojig://";
const ORIGIN: &str = "origin";
const OVERRIDE_COMMIT: &str = "override-commit";

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Refspec {
    kind: RefspecType,
    value: String,
}

struct Cache {
    refspec: Refspec,
    override_commit: Option<String>,
    #[allow(dead_code)]
    unconfigured_state: Option<String>,
    rojig_override_version: Option<String>,
    #[allow(dead_code)]
    rojig_description: Option<String>,

    packages: BTreeSet<String>,
    packages_local: BTreeMap<String, String>,
    override_remove: BTreeSet<String>,
    override_replace_local: BTreeMap<String, String>,

    initramfs_etc: BTreeSet<String>,
    initramfs_args: Vec<String>,
}

pub struct Origin {
    kf: KeyFile,
    cache: Cache,
}

fn keyfile_dup(kf: &KeyFile) -> KeyFile {
    let r = KeyFile::new();
    r.load_from_data(&kf.to_data(), glib::KeyFileFlags::KEEP_COMMENTS)
        .expect("keyfile parse");
    r
}

fn map_keyfile_optional<T>(res: StdResult<T, glib::Error>) -> StdResult<Option<T>, glib::Error> {
    match res {
        Ok(v) => Ok(Some(v)),
        Err(e) => {
            if let Some(t) = e.kind::<glib::KeyFileError>() {
                match t {
                    glib::KeyFileError::GroupNotFound | glib::KeyFileError::KeyNotFound => Ok(None),
                    _ => Err(e.into()),
                }
            } else {
                Err(e.into())
            }
        }
    }
}

fn parse_stringlist(kf: &KeyFile, group: &str, key: &str) -> Result<BTreeSet<String>> {
    let l = if let Some(l) = map_keyfile_optional(kf.get_string_list(group, key))? {
        l
    } else {
        return Ok(Default::default());
    };
    let mut r = BTreeSet::new();
    for it in l {
        r.insert(it.to_string());
    }
    Ok(r)
}

fn parse_localpkglist(kf: &KeyFile, group: &str, key: &str) -> Result<BTreeMap<String, String>> {
    let l = if let Some(l) = map_keyfile_optional(kf.get_string_list(group, key))? {
        l
    } else {
        return Ok(Default::default());
    };
    let mut r = BTreeMap::new();
    for it in l {
        let (nevra, sha256) = crate::utils::decompose_sha256_nevra(it.as_str())?;
        r.insert(nevra.to_string(), sha256.to_string());
    }
    Ok(r)
}

fn keyfile_get_optional_string(kf: &KeyFile, group: &str, key: &str) -> Result<Option<String>> {
    Ok(map_keyfile_optional(kf.get_value(group, key))?.map(|v| v.to_string()))
}

fn keyfile_get_nonempty_optional_string(
    kf: &KeyFile,
    group: &str,
    key: &str,
) -> Result<Option<String>> {
    if let Some(v) = keyfile_get_optional_string(&kf, group, key)? {
        if v.len() > 0 {
            return Ok(Some(v));
        }
    }
    Ok(None)
}

fn new_strmap<S: AsRef<str>>(c: impl Iterator<Item = (S, S)>) -> Vec<StringMapping> {
    c.map(|(k, v)| StringMapping {
        k: k.as_ref().to_string(),
        v: v.as_ref().to_string(),
    })
    .collect()
}

impl Origin {
    #[cfg(test)]
    fn new_from_str<S: AsRef<str>>(s: S) -> Result<Box<Self>> {
        let s = s.as_ref();
        let kf = glib::KeyFile::new();
        kf.load_from_data(s, glib::KeyFileFlags::KEEP_COMMENTS)?;
        Ok(Self::new_parse(&kf)?)
    }

    fn new_parse(kf: &KeyFile) -> Result<Box<Self>> {
        let kf = keyfile_dup(kf);
        let rojig = keyfile_get_optional_string(&kf, "origin", "rojig")?;
        let refspec_str = if let Some(r) = keyfile_get_optional_string(&kf, "origin", "refspec")? {
            Some(r)
        } else {
            keyfile_get_optional_string(&kf, "origin", "baserefspec")?
        };
        let refspec = match (refspec_str, rojig) {
            (Some(refspec), None) => {
                if ostree::validate_checksum_string(&refspec).is_ok() {
                    Refspec {
                        kind: RefspecType::Checksum,
                        value: refspec.clone(),
                    }
                } else {
                    Refspec {
                        kind: RefspecType::Ostree,
                        value: refspec.clone(),
                    }
                }
            },
            (None, Some(rojig)) => Refspec {
                kind: RefspecType::Rojig,
                value: rojig
            },
            (None, None) => bail!("No origin/refspec, origin/rojig, or origin/baserefspec in current deployment origin; cannot handle via rpm-ostree"),
            (Some(_), Some(_)) => bail!("Duplicate origin/refspec and origin/rojig in deployment origin"),
        };
        let override_commit = keyfile_get_optional_string(&kf, "origin", "override-commit")?;
        let unconfigured_state = keyfile_get_optional_string(&kf, "origin", "unconfigured-state")?;
        let packages = parse_stringlist(&kf, "packages", "requested")?;
        let packages_local = parse_localpkglist(&kf, "packages", "requested-local")?;
        let override_remove = parse_stringlist(&kf, "overrides", "remove")?;
        let override_replace_local = parse_localpkglist(&kf, "overrides", "replace-local")?;
        let initramfs_etc = parse_stringlist(&kf, "rpmostree", "initramfs-etc")?;
        let initramfs_args =
            map_keyfile_optional(kf.get_string_list("rpmostree", "initramfs-args"))?
                .map(|v| {
                    let r: Vec<String> = v.into_iter().map(|s| s.to_string()).collect();
                    r
                })
                .unwrap_or_default();
        let rojig_override_version =
            keyfile_get_optional_string(&kf, ORIGIN, "rojig-override-version")?;
        let rojig_description = keyfile_get_optional_string(&kf, ORIGIN, "rojig-description")?;
        Ok(Box::new(Self {
            kf,
            cache: Cache {
                refspec: refspec,
                override_commit,
                unconfigured_state,
                rojig_override_version,
                rojig_description,
                packages,
                packages_local,
                override_remove,
                override_replace_local,
                initramfs_etc,
                initramfs_args,
            },
        }))
    }

    /// Like clone() except returns a boxed value
    fn duplicate(&self) -> Box<Self> {
        // Unwrap safety - we know the internal keyfile is valid
        Self::new_parse(&self.kf).expect("valid keyfile internally")
    }
}

impl Origin {
    pub(crate) fn remove_transient_state(&mut self) {
        unsafe {
            ostree_sys::ostree_deployment_origin_remove_transient_state(self.kf.to_glib_none().0)
        }
        self.set_override_commit(None)
    }

    pub(crate) fn set_override_commit(&mut self, checksum: Option<(&str, Option<&str>)>) {
        match checksum {
            Some((checksum, ver)) => {
                self.kf.set_string(ORIGIN, OVERRIDE_COMMIT, checksum);
                self.cache.override_commit = Some(checksum.to_string());
                if let Some(ver) = ver {
                    let comment = format!("Version {}", ver);
                    // Ignore errors here, shouldn't happen
                    let _ =
                        self.kf
                            .set_comment(Some(ORIGIN), Some(OVERRIDE_COMMIT), comment.as_str());
                }
            }
            None => {
                // Ignore errors here, should only be failure to remove a nonexistent key.
                let _ = self.kf.remove_key(ORIGIN, OVERRIDE_COMMIT);
                self.cache.override_commit = None;
            }
        }
    }

    pub(crate) fn set_rojig_version(&mut self, version: Option<&str>) {
        match version {
            Some(version) => {
                self.kf
                    .set_string(ORIGIN, "rojig-override-version", version);
                self.cache.rojig_override_version = Some(version.to_string());
            }
            None => {
                let _ = self.kf.remove_key(ORIGIN, "rojig-override-version");
                self.cache.rojig_override_version = None;
            }
        }
    }

    pub(crate) fn get_refspec_type(&self) -> RefspecType {
        self.cache.refspec.kind
    }

    pub(crate) fn get_prefixed_refspec(&self) -> String {
        let val = self.cache.refspec.value.as_str();
        match self.cache.refspec.kind {
            RefspecType::Rojig => format!("{}{}", ROJIG_PREFIX, val),
            _ => val.to_string(),
        }
    }

    pub(crate) fn get_packages(&self) -> Vec<String> {
        self.cache.packages.iter().cloned().collect()
    }

    pub(crate) fn get_local_packages(&self) -> Vec<StringMapping> {
        new_strmap(self.cache.packages_local.iter())
    }

    pub(crate) fn get_override_remove(&self) -> Vec<String> {
        self.cache.override_remove.iter().cloned().collect()
    }

    pub(crate) fn get_override_local_replace(&self) -> Vec<StringMapping> {
        new_strmap(self.cache.override_replace_local.iter())
    }

    pub(crate) fn get_custom_url(&self) -> Result<String> {
        // FIXME(cxx-rs) propagate Option once supported
        Ok(
            keyfile_get_nonempty_optional_string(&self.kf, "origin", "custom-url")?
                .unwrap_or_default(),
        )
    }

    pub(crate) fn get_custom_description(&self) -> Result<String> {
        // FIXME(cxx-rs) propagate Option once supported
        Ok(
            keyfile_get_nonempty_optional_string(&self.kf, "origin", "custom-description")?
                .unwrap_or_default(),
        )
    }

    pub(crate) fn get_rojig_description(&self) -> String {
        // FIXME(cxx-rs) propagate Option once supported
        self.cache
            .rojig_description
            .as_ref()
            .map(String::from)
            .unwrap_or_default()
    }

    pub(crate) fn get_regenerate_initramfs(&self) -> bool {
        match map_keyfile_optional(self.kf.get_boolean("rpmostree", "regenerate-initramfs")) {
            Ok(Some(v)) => v,
            Ok(None) => false,
            Err(_) => false, // FIXME Should propagate errors here in the future
        }
    }

    pub(crate) fn get_initramfs_etc_files(&self) -> Vec<String> {
        self.cache.initramfs_etc.iter().cloned().collect()
    }

    pub(crate) fn get_initramfs_args(&self) -> Vec<String> {
        self.cache.initramfs_args.clone()
    }

    pub(crate) fn may_require_local_assembly(&self) -> bool {
        self.get_regenerate_initramfs()
            || !self.cache.initramfs_etc.is_empty()
            || !self.cache.packages.is_empty()
            || !self.cache.packages_local.is_empty()
            || !self.cache.override_replace_local.is_empty()
            || !self.cache.override_remove.is_empty()
    }

    pub(crate) fn is_rojig(&self) -> bool {
        self.cache.refspec.kind == RefspecType::Rojig
    }

    // Binding for cxx
    pub(crate) fn get_override_local_pkgs(&self) -> Vec<String> {
        let mut r = Vec::new();
        for v in self.cache.override_replace_local.values() {
            r.push(v.clone())
        }
        r
    }
}

pub(crate) fn origin_parse_deployment(
    mut deployment: Pin<&mut FFIOstreeDeployment>,
) -> CxxResult<Box<Origin>> {
    let deployment = deployment.gobj_wrap();
    let kf = deployment
        .get_origin()
        .ok_or_else(|| anyhow!("No origin found for deployment"))?;
    Ok(Origin::new_parse(&kf)?)
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_basic() -> Result<()> {
        let o = Origin::new_from_str(
            "[origin]
refspec=foo:bar/x86_64/baz
",
        )?;
        assert_eq!(o.cache.refspec.kind, RefspecType::Ostree);
        assert_eq!(o.cache.refspec.value, "foo:bar/x86_64/baz");
        assert_eq!(o.cache.packages.len(), 0);
        // Call various methods so we have test coverage, but also
        // to suppress dead code until we actually start using this
        // instead of the C++ code.
        assert_eq!(o.get_refspec_type(), RefspecType::Ostree);
        assert!(!o.may_require_local_assembly());
        assert!(!o.get_regenerate_initramfs());
        assert!(o.get_initramfs_etc_files().is_empty());
        assert!(!o.is_rojig());
        assert_eq!(o.get_rojig_description(), "");
        assert_eq!(o.get_custom_url()?, "");
        assert_eq!(o.get_custom_description()?, "");
        assert!(o.get_override_local_pkgs().is_empty());
        assert_eq!(o.get_prefixed_refspec(), "foo:bar/x86_64/baz");
        // Mutation methods
        let mut o = o.duplicate();
        assert_eq!(o.get_refspec_type(), RefspecType::Ostree);
        o.remove_transient_state();
        o.set_rojig_version(Some("42"));

        let mut o = Origin::new_from_str(
            r#"
[origin]
baserefspec=fedora/33/x86_64/silverblue

[packages]
requested=virt-manager;libvirt;pcsc-lite-ccid
"#,
        )?;
        assert_eq!(o.cache.refspec.kind, RefspecType::Ostree);
        assert_eq!(o.cache.refspec.value, "fedora/33/x86_64/silverblue");
        assert!(o.may_require_local_assembly());
        assert!(!o.get_regenerate_initramfs());
        assert_eq!(o.cache.packages.len(), 3);
        assert!(o.cache.packages.contains("libvirt"));

        let override_commit = (
            "126539c731acf376359aced177dc5dff598dd6714a0a8faf753c727559adc8b5",
            Some("42.3"),
        );
        assert!(o.cache.override_commit.is_none());
        o.set_override_commit(Some(override_commit));
        assert_eq!(
            o.cache.override_commit.as_ref().expect("override"),
            override_commit.0
        );
        Ok(())
    }
}
