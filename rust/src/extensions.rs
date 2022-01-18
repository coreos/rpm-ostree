//! Core logic for extensions.yaml file.

/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use anyhow::{bail, Context, Result};
use openat_ext::OpenatDirExt;
use serde_derive::{Deserialize, Serialize};
use std::collections::HashMap;

use crate::cxxrsutil::*;
use crate::ffi::StringMapping;
use crate::utils;
use crate::Treefile;
use crate::{BaseComposeConfigFields, TreeComposeConfig};

const RPMOSTREE_EXTENSIONS_STATE_FILE: &str = ".rpm-ostree-state-chksum";

#[derive(Serialize, Deserialize, Debug)]
#[serde(rename_all = "kebab-case")]
pub struct Extensions {
    extensions: HashMap<String, Extension>,
    #[serde(skip_serializing_if = "Option::is_none")]
    repos: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    modules: Option<crate::treefile::ModulesConfig>,
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(rename_all = "kebab-case")]
pub struct Extension {
    packages: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    repos: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    modules: Option<crate::treefile::ModulesConfig>,
    #[serde(skip_serializing_if = "Option::is_none")]
    architectures: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    match_base_evr: Option<String>,
    #[serde(default)]
    kind: ExtensionKind,
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
enum ExtensionKind {
    OsExtension,
    Development,
}

impl Default for ExtensionKind {
    fn default() -> Self {
        ExtensionKind::OsExtension
    }
}

fn extensions_load_stream(
    stream: &mut impl std::io::Read,
    basearch: &str,
    base_pkgs: &[StringMapping],
) -> Result<Box<Extensions>> {
    let mut parsed: Extensions = serde_yaml::from_reader(stream)?;

    parsed.extensions.retain(|_, ext| {
        ext.architectures
            .as_ref()
            .map(|v| v.iter().any(|a| a == basearch))
            .unwrap_or(true)
    });

    let base_pkgs: HashMap<&str, &str> = base_pkgs
        .iter()
        .map(|i| (i.k.as_str(), i.v.as_str()))
        .collect();

    for (_, ext) in parsed.extensions.iter_mut() {
        // Extend the global extension state with the per-extension repos and modules,
        // after we've done architecture filtering.
        if let Some(repos) = parsed.repos.as_mut() {
            repos.extend(ext.repos.take().unwrap_or_default());
        } else {
            parsed.repos = ext.repos.take();
        }
        crate::treefile::merge_modules(&mut parsed.modules, &mut ext.modules);
        if ext.kind == ExtensionKind::OsExtension {
            for pkg in &ext.packages {
                if base_pkgs.contains_key(pkg.as_str()) {
                    bail!("package {} already present in base", pkg);
                }
            }
        }
        if let Some(ref matched_base_pkg) = ext.match_base_evr {
            let evr = base_pkgs
                .get(matched_base_pkg.as_str())
                .with_context(|| format!("couldn't find base package {}", matched_base_pkg))?;
            let pkgs = ext
                .packages
                .iter()
                .map(|pkg| format!("{}-{}", pkg, evr))
                .collect();
            ext.packages = pkgs;
        }
    }

    Ok(Box::new(parsed))
}

pub(crate) fn extensions_load(
    path: &str,
    basearch: &str,
    base_pkgs: &Vec<StringMapping>,
) -> CxxResult<Box<Extensions>> {
    let f = utils::open_file(path)?;
    let mut f = std::io::BufReader::new(f);
    Ok(extensions_load_stream(&mut f, basearch, base_pkgs)
        .with_context(|| format!("parsing {}", path))?)
}

impl Extensions {
    pub(crate) fn get_repos(&self) -> Vec<String> {
        self.repos.as_ref().cloned().unwrap_or_default()
    }

    pub(crate) fn get_os_extension_packages(&self) -> Vec<String> {
        self.extensions
            .iter()
            .filter(|(_, ext)| ext.kind == ExtensionKind::OsExtension)
            .flat_map(|(_, ext)| ext.packages.iter().cloned())
            .collect()
    }

    pub(crate) fn get_development_packages(&self) -> Vec<String> {
        self.extensions
            .iter()
            .filter(|(_, ext)| ext.kind == ExtensionKind::Development)
            .flat_map(|(_, ext)| ext.packages.iter().cloned())
            .collect()
    }

    pub(crate) fn state_checksum_changed(&self, chksum: &str, output_dir: &str) -> CxxResult<bool> {
        let output_dir = openat::Dir::open(output_dir)?;
        if let Some(prev_chksum) =
            output_dir.read_to_string_optional(RPMOSTREE_EXTENSIONS_STATE_FILE)?
        {
            Ok(prev_chksum != chksum)
        } else {
            Ok(true)
        }
    }

    pub(crate) fn update_state_checksum(&self, chksum: &str, output_dir: &str) -> CxxResult<()> {
        let output_dir = openat::Dir::open(output_dir)?;
        Ok(output_dir
            .write_file_contents(RPMOSTREE_EXTENSIONS_STATE_FILE, 0o644, chksum)
            .with_context(|| format!("updating state file {}", RPMOSTREE_EXTENSIONS_STATE_FILE))?)
    }

    pub(crate) fn serialize_to_dir(&self, output_dir: &str) -> CxxResult<()> {
        let output_dir = openat::Dir::open(output_dir)?;
        Ok(output_dir
            .write_file_with("extensions.json", 0o644, |w| -> Result<_> {
                Ok(serde_json::to_writer_pretty(w, self)?)
            })
            .context("while serializing")?)
    }

    /// Create a treefile representing just what needs to be installed for extensions.
    pub(crate) fn generate_treefile(&self, src: &Treefile) -> CxxResult<Box<Treefile>> {
        let mut repos = src.parsed.base.repos.clone().unwrap_or_default();
        repos.extend(self.repos.iter().flatten().cloned());
        let ret = TreeComposeConfig {
            base: BaseComposeConfigFields {
                repos: Some(repos),
                releasever: src.parsed.base.releasever.clone(),
                ..Default::default()
            },
            packages: Some(self.get_os_extension_packages()),
            modules: self.modules.clone(),
            ..Default::default()
        };
        Ok(Box::new(Treefile::new_from_config(ret, None)?))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn base_rpmdb() -> Vec<StringMapping> {
        vec![
            StringMapping {
                k: "systemd".into(),
                v: "246.9-3".into(),
            },
            StringMapping {
                k: "foobar".into(),
                v: "1.2-3".into(),
            },
        ]
    }

    #[test]
    fn basic() {
        let buf = r###"
repos:
    - my-repo
extensions:
    bazboo:
        packages:
            - bazboo
"###;
        let mut input = std::io::BufReader::new(buf.as_bytes());
        let extensions = extensions_load_stream(&mut input, "x86_64", &base_rpmdb()).unwrap();
        assert!(extensions.get_repos() == vec!["my-repo"]);
        assert!(extensions.get_os_extension_packages() == vec!["bazboo"]);
        assert!(extensions.get_development_packages().is_empty());

        let src_tf =
            crate::Treefile::new_from_string(crate::treefile::tests::VALID_PRELUDE_JS).unwrap();
        let tf = extensions.generate_treefile(&src_tf).unwrap();

        assert_ne!(src_tf.parsed, tf.parsed);

        let allrepos: Vec<_> = tf
            .parsed
            .base
            .repos
            .as_ref()
            .unwrap()
            .iter()
            .map(|s| s.as_str())
            .collect();
        // rpm-md repos are unioned
        assert_eq!(allrepos, vec!["baserepo", "my-repo"]);
        // packages are reset to just extension packages
        assert_eq!(tf.parsed.packages.as_ref().unwrap().len(), 1);
        assert_eq!(
            tf.parsed.packages.iter().flatten().next().unwrap(),
            "bazboo"
        );
        // Other bits shouldn't be there.
        assert!(src_tf.parsed.base.treeref.is_some());
        assert!(tf.parsed.base.treeref.is_none());
    }

    #[test]
    fn ext_in_base() {
        let buf = r###"
extensions:
    foobar:
        packages:
            - foobar
"###;
        let mut input = std::io::BufReader::new(buf.as_bytes());
        match extensions_load_stream(&mut input, "x86_64", &base_rpmdb()) {
            Ok(_) => panic!("expected failure from extension in base"),
            Err(ref e) => assert!(e.to_string() == "package foobar already present in base"),
        }
    }

    #[test]
    fn ext_in_devel() {
        let buf = r###"
extensions:
    foobar:
        packages:
            - foobar
        kind: development
"###;
        let mut input = std::io::BufReader::new(buf.as_bytes());
        let extensions = extensions_load_stream(&mut input, "x86_64", &base_rpmdb()).unwrap();
        assert!(extensions.get_os_extension_packages().is_empty());
        assert!(extensions.get_development_packages() == vec!["foobar"]);
    }

    #[test]
    fn basearch_filter() {
        let buf = r###"
repos:
    - baserepo
modules:
    enable:
        - virt:av
extensions:
    bazboo:
        packages:
            - bazboo
        repos:
            - bazboo-repo
        architectures:
            - x86_64
    dodo:
        packages:
            - dodo
            - dada
        architectures:
            - s390x
    foo:
        modules:
            enable:
                - foo:stable
        packages:
            - foo
        repos:
            - foo-repo
        architectures:
            - ppc64le
"###;
        let mut input = std::io::BufReader::new(buf.as_bytes());
        let extensions = extensions_load_stream(&mut input, "x86_64", &base_rpmdb()).unwrap();
        assert!(extensions.get_os_extension_packages() == vec!["bazboo"]);
        assert_eq!(extensions.get_repos().len(), 2);
        assert_eq!(extensions.get_repos()[1], "bazboo-repo");
        let modules = extensions.modules.unwrap();
        assert_eq!(modules.enable.unwrap(), vec!["virt:av"]);
        assert!(modules.install.is_none());

        let mut input = std::io::BufReader::new(buf.as_bytes());
        let extensions = extensions_load_stream(&mut input, "s390x", &base_rpmdb()).unwrap();
        assert!(extensions.get_os_extension_packages() == vec!["dodo", "dada"]);
        assert_eq!(extensions.get_repos().len(), 1);
        assert_eq!(extensions.get_repos()[0], "baserepo");

        let mut input = std::io::BufReader::new(buf.as_bytes());
        let extensions = extensions_load_stream(&mut input, "ppc64le", &base_rpmdb()).unwrap();
        assert_eq!(extensions.get_os_extension_packages(), vec!["foo"]);
        assert_eq!(extensions.get_repos().len(), 2);
        assert_eq!(extensions.get_repos()[1], "foo-repo");
        let modules = extensions.modules.unwrap();
        assert_eq!(modules.enable.unwrap(), vec!["foo:stable", "virt:av"]);
        assert!(modules.install.is_none());
    }

    #[test]
    fn matching_evr() {
        let buf = r###"
extensions:
    foobar-ext:
        packages:
            - foobar-ext
        match-base-evr: foobar
        kind: os-extension
    devel:
        packages:
            - foobar-devel
        match-base-evr: foobar
        kind: development
"###;
        let mut input = std::io::BufReader::new(buf.as_bytes());
        let extensions = extensions_load_stream(&mut input, "x86_64", &base_rpmdb()).unwrap();
        assert!(extensions.get_os_extension_packages() == vec!["foobar-ext-1.2-3"]);
        assert!(extensions.get_development_packages() == vec!["foobar-devel-1.2-3"]);
    }
}
