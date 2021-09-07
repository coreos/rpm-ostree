//! An "origin" declares how we generated an OSTree commit.

/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use crate::cxxrsutil::*;
use crate::treefile::Treefile;
use anyhow::Result;
use fn_error_context::context;
use glib::KeyFile;
use ostree_ext::glib;
use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::pin::Pin;
use std::result::Result as StdResult;

const ORIGIN: &str = "origin";
const RPMOSTREE: &str = "rpmostree";
const PACKAGES: &str = "packages";
const MODULES: &str = "modules";
const OVERRIDES: &str = "overrides";

/// The set of keys that we parse as BTreeMap and need to ignore ordering changes.
static UNORDERED_LIST_KEYS: phf::Set<&'static str> = phf::phf_set! {
    "packages/local",
    "overrides/replace-local"
};

#[context("Parsing origin")]
pub(crate) fn origin_to_treefile_inner(kf: &KeyFile) -> Result<Box<Treefile>> {
    let mut cfg: crate::treefile::TreeComposeConfig = Default::default();
    let refspec_str = if let Some(r) = keyfile_get_optional_string(kf, ORIGIN, "refspec")? {
        Some(r)
    } else if let Some(r) = keyfile_get_optional_string(kf, ORIGIN, "baserefspec")? {
        Some(r)
    } else {
        keyfile_get_optional_string(kf, ORIGIN, "container-image-reference")?
    };
    let refspec_str = refspec_str.ok_or_else(|| {
        anyhow::anyhow!("Failed to find refspec/baserefspec/container-image-reference in origin")
    })?;
    cfg.derive.base_refspec = Some(refspec_str);
    cfg.packages = parse_stringlist(kf, PACKAGES, "requested")?;
    cfg.derive.packages_local = parse_localpkglist(kf, PACKAGES, "requested-local")?;
    let modules_enable = parse_stringlist(kf, MODULES, "enable")?;
    let modules_install = parse_stringlist(kf, MODULES, "install")?;
    if modules_enable.is_some() || modules_install.is_some() {
        cfg.modules = Some(crate::treefile::ModulesConfig {
            enable: modules_enable,
            install: modules_install,
        });
    }
    cfg.derive.override_remove = parse_stringlist(kf, OVERRIDES, "remove")?;
    cfg.derive.override_replace_local = parse_localpkglist(kf, OVERRIDES, "replace-local")?;

    let regenerate_initramfs = kf
        .boolean(RPMOSTREE, "regenerate-initramfs")
        .unwrap_or_default();
    let initramfs_etc = parse_stringlist(kf, RPMOSTREE, "initramfs-etc")?;
    let initramfs_args = parse_stringlist(kf, RPMOSTREE, "initramfs-args")?;
    if regenerate_initramfs || initramfs_etc.is_some() || initramfs_args.is_some() {
        let initramfs = crate::treefile::DeriveInitramfs {
            regenerate: regenerate_initramfs,
            etc: initramfs_etc,
            args: initramfs_args,
        };
        cfg.derive.initramfs = Some(initramfs);
    }

    if let Some(url) = keyfile_get_optional_string(kf, ORIGIN, "custom-url")? {
        let description = keyfile_get_optional_string(kf, ORIGIN, "custom-description")?;
        cfg.derive.custom = Some(crate::treefile::DeriveCustom { url, description })
    }

    if map_keyfile_optional(kf.boolean(RPMOSTREE, "ex-cliwrap"))?.unwrap_or_default() {
        cfg.cliwrap = Some(true)
    }

    cfg.derive.override_commit = keyfile_get_optional_string(kf, ORIGIN, "override-commit")?;

    Ok(Box::new(Treefile::new_from_config(cfg, None)?))
}

/// Convert an origin keyfile to a treefile config.
///
/// For historical reasons, rpm-ostree has two file formats to represent
/// state.  This bridges parts of an origin file to a treefile that
/// is understood by the core.
pub(crate) fn origin_to_treefile(
    mut kf: Pin<&mut crate::ffi::GKeyFile>,
) -> CxxResult<Box<Treefile>> {
    let kf = kf.gobj_wrap();
    Ok(origin_to_treefile_inner(&kf)?)
}

/// Set a keyfile value to a string list.
fn kf_set_string_list<'a>(
    kf: &glib::KeyFile,
    group: impl AsRef<str>,
    k: impl AsRef<str>,
    vals: impl IntoIterator<Item = &'a str>,
) {
    let mut v = String::new();
    for elt in vals {
        v.push_str(elt);
        v.push(';');
    }
    kf.set_value(group.as_ref(), k.as_ref(), v.as_str())
}

fn set_sha256_nevra_pkgs(
    kf: &glib::KeyFile,
    group: &str,
    k: &str,
    pkgs: &BTreeMap<String, String>,
) {
    let pkgs: Vec<_> = pkgs
        .iter()
        .map(|(nevra, sha256)| format!("{}:{}", sha256, nevra))
        .collect();
    let pkgs = pkgs.iter().map(|s| s.as_str());
    kf_set_string_list(&kf, group, k, pkgs)
}

/// Convert a treefile to an origin file.
#[context("Parsing treefile origin")]
fn treefile_to_origin_inner(tf: &Treefile) -> Result<glib::KeyFile> {
    let tf = &tf.parsed;
    let kf = glib::KeyFile::new();

    // refspec (note special handling right now for layering)
    let deriving = tf.packages.is_some() || tf.derive.packages_local.is_some();
    if let Some(r) = tf.derive.base_refspec.as_deref() {
        let k = if deriving { "baserefspec" } else { "refspec" };
        kf.set_string(ORIGIN, k, r)
    };

    // Packages
    if let Some(pkgs) = tf.packages.as_deref() {
        let pkgs = pkgs.iter().map(|s| s.as_str());
        kf_set_string_list(&kf, PACKAGES, "requested", pkgs)
    }
    if let Some(pkgs) = tf.derive.packages_local.as_ref() {
        set_sha256_nevra_pkgs(&kf, PACKAGES, "requested-local", pkgs)
    }
    if let Some(pkgs) = tf.derive.override_remove.as_deref() {
        let pkgs = pkgs.iter().map(|s| s.as_str());
        kf_set_string_list(&kf, OVERRIDES, "remove", pkgs)
    }
    if let Some(pkgs) = tf.derive.override_replace_local.as_ref() {
        set_sha256_nevra_pkgs(&kf, OVERRIDES, "replace-local", pkgs)
    }
    if let Some(ref modcfg) = tf.modules {
        if let Some(modules) = modcfg.enable.as_deref() {
            let modules = modules.iter().map(|s| s.as_str());
            kf_set_string_list(&kf, MODULES, "enable", modules)
        }
        if let Some(modules) = modcfg.install.as_deref() {
            let modules = modules.iter().map(|s| s.as_str());
            kf_set_string_list(&kf, MODULES, "install", modules)
        }
    }

    // Initramfs bits
    if let Some(initramfs) = tf.derive.initramfs.as_ref() {
        if initramfs.regenerate {
            kf.set_boolean(RPMOSTREE, "regenerate-initramfs", true);
        }
        if let Some(etc) = initramfs.etc.as_deref() {
            let etc = etc.iter().map(|s| s.as_str());
            kf_set_string_list(&kf, RPMOSTREE, "initramfs-etc", etc)
        }
        if let Some(args) = initramfs.args.as_deref() {
            let args = args.iter().map(|s| s.as_str());
            kf_set_string_list(&kf, RPMOSTREE, "initramfs-args", args)
        }
    }

    // Custom origin
    if let Some(custom) = tf.derive.custom.as_ref() {
        kf.set_string(ORIGIN, "custom-url", custom.url.as_str());
        if let Some(desc) = custom.description.as_deref() {
            kf.set_string(ORIGIN, "custom-description", desc);
        }
    }

    if tf.cliwrap.unwrap_or_default() {
        kf.set_boolean(RPMOSTREE, "ex-cliwrap", true)
    }

    if let Some(c) = tf.derive.override_commit.as_deref() {
        kf.set_string(ORIGIN, "override-commit", c);
    }

    Ok(kf)
}

fn kf_diff_value(group: &str, key: &str, a: &str, b: &str) -> bool {
    if a == b {
        return true;
    }
    let path = format!("{}/{}", group, key);
    if !UNORDERED_LIST_KEYS.contains(path.as_str()) {
        return false;
    }
    let a: BTreeSet<_> = a.split(';').collect();
    let b: BTreeSet<_> = b.split(';').collect();
    a == b
}

/// Diff two key files.
fn kf_diff(kf: &glib::KeyFile, newkf: &glib::KeyFile) -> Result<()> {
    let mut errs = Vec::new();
    for grp in kf.groups().0.iter().map(|g| g.as_str()) {
        for k in kf.keys(grp)?.0.iter().map(|g| g.as_str()) {
            let origv = kf.value(grp, k)?;
            match newkf.value(grp, k) {
                Ok(newv) => {
                    if !kf_diff_value(grp, k, origv.as_str(), newv.as_str()) {
                        errs.push(format!("Mismatched value for {}/{}: {}", grp, k, newv));
                    }
                }
                Err(e) => errs.push(format!("Fetching {}/{}: {}", grp, k, e)),
            }
        }
    }
    for grp in newkf.groups().0.iter().map(|g| g.as_str()) {
        for k in newkf.keys(grp)?.0.iter().map(|g| g.as_str()) {
            if !kf.has_key(grp, k)? {
                errs.push(format!("Unexpected new key: {}/{}", grp, k));
            }
        }
    }
    if !errs.is_empty() {
        return Err(anyhow::anyhow!(errs.join("; ")));
    }
    Ok(())
}

fn origin_validate_roundtrip_inner(kf: &glib::KeyFile) -> Result<()> {
    // Make a copy of our input so we can remove the transient fields.
    let kf_copy = glib::KeyFile::new();
    kf_copy.load_from_data(kf.to_data().as_str(), glib::KeyFileFlags::NONE)?;
    let kf = kf_copy;
    // We don't translate transient fields
    drop(kf.remove_group("libostree-transient"));

    let tf = origin_to_treefile_inner(&kf)?;
    let newkf = treefile_to_origin_inner(&tf)?;
    // Compare the two origin keyfiles.  This is the core check.
    kf_diff(&kf, &newkf)?;
    // And finally, triple-check things by round-tripping the origin
    // back to a treefile and asserting it's identical.
    // At the moment, we don't accept user-supplied treefiles as input
    // to this code.  For now we fatally error if somehow they differed.
    // But in the future this check should be part of validating treefile
    // options that don't make sense on the client side.
    let newtf = origin_to_treefile_inner(&newkf)?;
    assert_eq!(tf.parsed, newtf.parsed);
    Ok(())
}

/// Validate that an origin keyfile can be losslessly converted to a treefile config.
///
/// For historical reasons, rpm-ostree has two file formats to represent
/// state.  This bridges parts of an origin file to a treefile that
/// is understood by the core.
pub(crate) fn origin_validate_roundtrip(mut kf: Pin<&mut crate::ffi::GKeyFile>) {
    let kf = kf.gobj_wrap();
    origin_validate_roundtrip_inner(&kf).err().map(|e| {
        tracing::debug!("Failed to roundtrip origin: {}", e);
    });
}

fn map_keyfile_optional<T>(res: StdResult<T, glib::Error>) -> StdResult<Option<T>, glib::Error> {
    match res {
        Ok(v) => Ok(Some(v)),
        Err(e) => {
            if let Some(t) = e.kind::<glib::KeyFileError>() {
                match t {
                    glib::KeyFileError::GroupNotFound | glib::KeyFileError::KeyNotFound => Ok(None),
                    _ => Err(e),
                }
            } else {
                Err(e)
            }
        }
    }
}

fn parse_stringlist(kf: &KeyFile, group: &str, key: &str) -> Result<Option<Vec<String>>> {
    let r = map_keyfile_optional(kf.string_list(group, key))?
        .map(|o| o.into_iter().map(|s| s.to_string()).collect());
    Ok(r)
}

fn parse_localpkglist(
    kf: &KeyFile,
    group: &str,
    key: &str,
) -> Result<Option<BTreeMap<String, String>>> {
    if let Some(v) = map_keyfile_optional(kf.string_list(group, key))? {
        let mut r = BTreeMap::new();
        for s in v {
            let (nevra, sha256) = crate::utils::decompose_sha256_nevra(s.as_str())?;
            r.insert(nevra.to_string(), sha256.to_string());
        }
        Ok(Some(r))
    } else {
        Ok(None)
    }
}

fn keyfile_get_optional_string(kf: &KeyFile, group: &str, key: &str) -> Result<Option<String>> {
    Ok(map_keyfile_optional(kf.string(group, key))?.map(|v| v.to_string()))
}

#[cfg(test)]
pub(crate) mod test {
    use super::*;
    use indoc::indoc;

    macro_rules! assert_err_containing {
        ( $e:expr, $expected_msg:expr ) => {{
            let msg = $e.unwrap_err().to_string();
            let expected = $expected_msg;
            if !msg.contains(expected) {
                panic!("Expected error to contain {}\nfound: {}", expected, msg)
            }
        }};
    }

    pub(crate) const BASE: &str = indoc! {"
    [origin]
    refspec=foo:bar/x86_64/baz
    "};

    pub(crate) const COMPLEX: &str = indoc! {"
    [origin]
    baserefspec=fedora:fedora/34/x86_64/silverblue
    override-commit=41af286dc0b172ed2f1ca934fd2278de4a1192302ffa07087cea2682e7d372e3

    [rpmostree]
    regenerate-initramfs=true
    initramfs-args=-I;/etc/foobar.conf;
    initramfs-etc=/etc/cmdline.d/foobar.conf;

    [packages]
    requested=libvirt;fish;
    requested-local=4ed748ba060fce4571e7ef19f3f5ed6209f67dbac8327af0d38ea70b96d2f723:foo-1.2-3.x86_64;

    [modules]
    enable=foo:2.0;bar:rolling;
    install=baz:next/development;

    [overrides]
    remove=docker;
    replace-local=0c7072500af2758e7dc7d7700fed82c3c5f4da7453b4d416e79f75384eee96b0:rpm-ostree-devel-2021.1-2.fc33.x86_64;648ab3ff4d4b708ea180269297de5fa3e972f4481d47b7879c6329272e474d68:rpm-ostree-2021.1-2.fc33.x86_64;8b29b78d0ade6ec3aedb8e3846f036f6f28afe64635d83cb6a034f1004607678:rpm-ostree-libs-2021.1-2.fc33.x86_64;

    [libostree-transient]
    pinned=true
    "};

    pub(crate) fn kf_from_str(s: &str) -> Result<glib::KeyFile> {
        let kf = glib::KeyFile::new();
        kf.load_from_data(s, glib::KeyFileFlags::KEEP_COMMENTS)?;
        Ok(kf)
    }

    #[test]
    fn test_kf_diff() -> Result<()> {
        let kf = kf_from_str(BASE)?;
        let kf2 = kf_from_str(BASE)?;
        kf_diff(&kf, &kf2).expect("No difference");
        kf2.set_string(ORIGIN, "refspec", "foo:bar/x86_64/whee");
        assert_err_containing!(kf_diff(&kf, &kf2), "Mismatched value");
        let kf2 = kf_from_str(BASE)?;
        kf2.set_string(ORIGIN, "foospec", "foo:bar/x86_64/whee");
        assert_err_containing!(kf_diff(&kf, &kf2), "Unexpected new key: origin/foospec");
        Ok(())
    }

    #[test]
    fn test_origin_parse() -> Result<()> {
        let kf = kf_from_str("[origin]\n")?;
        assert!(origin_to_treefile_inner(&kf).is_err());

        let kf = kf_from_str(BASE)?;
        let tf = origin_to_treefile_inner(&kf)?;
        assert_eq!(
            tf.parsed.derive.base_refspec.as_ref().unwrap(),
            "foo:bar/x86_64/baz"
        );

        let kf = kf_from_str(indoc! {"
            [origin]
            baserefspec=fedora/33/x86_64/silverblue

            [packages]
            requested=virt-manager;libvirt;pcsc-lite-ccid
        "})?;
        let tf = origin_to_treefile_inner(&kf)?;
        assert_eq!(
            tf.parsed.derive.base_refspec.as_ref().unwrap(),
            "fedora/33/x86_64/silverblue"
        );
        let pkgs = tf.parsed.packages.as_ref().unwrap();
        assert_eq!(pkgs.len(), 3);
        assert_eq!(pkgs[1], "libvirt");

        let kf = kf_from_str(COMPLEX)?;
        let tf = origin_to_treefile_inner(&kf)?;
        assert_eq!(
            tf.parsed.derive.override_commit.unwrap(),
            "41af286dc0b172ed2f1ca934fd2278de4a1192302ffa07087cea2682e7d372e3"
        );
        assert_eq!(
            tf.parsed.modules,
            Some(crate::treefile::ModulesConfig {
                enable: Some(vec!["foo:2.0".into(), "bar:rolling".into()]),
                install: Some(vec!["baz:next/development".into()]),
            })
        );
        Ok(())
    }

    #[test]
    fn test_origin_roundtrip() -> Result<()> {
        let kf = kf_from_str(BASE)?;
        origin_validate_roundtrip_inner(&kf).expect("validating BASE");
        let kf = kf_from_str(COMPLEX)?;
        origin_validate_roundtrip_inner(&kf).expect("validating COMPLEX");
        Ok(())
    }
}
