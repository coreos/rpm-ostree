//! Rust portion of rpmostreed-deployment-utils.cxx
//! The code here is mainly involved in converting on-disk state (i.e. ostree commits/deployments)
//! into GVariant which will be serialized via DBus.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::{
    PackageOverrideSource, PackageOverrideSourceKind, ParsedRevision, ParsedRevisionKind,
};
use anyhow::{anyhow, format_err, Result};
use cap_std_ext::cap_std;
use cap_std_ext::dirext::CapStdExtDirExt;
use cap_std_ext::rustix::fs::MetadataExt;
use fn_error_context::context;
use glib::prelude::*;
use openat_ext::OpenatDirExt;
use ostree_ext::{gio, glib, ostree};
use std::collections::BTreeMap;
use std::io::Read;
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::pin::Pin;

const RPM_OSTREED_COMMIT_VERIFICATION_CACHE: &str = "rpm-ostree/gpgcheck-cache";

/// Validate basic assumptions on daemon startup.
pub(crate) fn daemon_sanitycheck_environment(
    mut sysroot: Pin<&mut crate::FFIOstreeSysroot>,
) -> CxxResult<()> {
    let sysroot = &sysroot.gobj_wrap();
    let sysroot_dir = openat::Dir::open(format!("/proc/self/fd/{}", sysroot.fd()))?;
    let loc = crate::composepost::TRADITIONAL_RPMDB_LOCATION;
    if let Some(metadata) = sysroot_dir.metadata_optional(loc)? {
        let t = metadata.simple_type();
        if t != openat::SimpleType::Symlink {
            return Err(
                anyhow::anyhow!("/{} must be a symbolic link, but is: {:?}", loc, t).into(),
            );
        }
    }
    Ok(())
}

/// Get a currently unique (for this host) identifier for the
/// deployment; TODO - adding the deployment timestamp would make it
/// persistently unique, needs API in libostree.
pub(crate) fn deployment_generate_id(
    mut deployment: Pin<&mut crate::FFIOstreeDeployment>,
) -> String {
    let deployment = deployment.gobj_wrap();
    // unwrap safety: These can't actually return NULL
    format!(
        "{}-{}.{}",
        deployment.osname().unwrap(),
        deployment.csum().unwrap(),
        deployment.deployserial()
    )
}

/// Insert values from `v` into the target `dict` with key `k`.
fn vdict_insert_strv<'a>(dict: &glib::VariantDict, k: &str, v: impl IntoIterator<Item = &'a str>) {
    // TODO: drive this into variant_utils in ostree-rs-ext so we don't need
    // to allocate the intermediate vector.
    let v: Vec<_> = v.into_iter().collect();
    dict.insert_value(k, &v.to_variant());
}

/// Insert values from `v` into the target `dict` with key `k`.
fn vdict_insert_optvec(dict: &glib::VariantDict, k: &str, v: Option<&Vec<String>>) {
    let v = v.iter().map(|s| s.iter()).flatten().map(|s| s.as_str());
    vdict_insert_strv(dict, k, v);
}

/// Insert keys from the provided map into the target `dict` with key `k`.
fn vdict_insert_optmap(dict: &glib::VariantDict, k: &str, v: Option<&BTreeMap<String, String>>) {
    let v = v.iter().map(|s| s.keys()).flatten().map(|s| s.as_str());
    vdict_insert_strv(dict, k, v);
}

/// Insert into `dict` metadata keys derived from `origin`.
fn deployment_populate_variant_origin(
    origin: &glib::KeyFile,
    dict: &glib::VariantDict,
) -> Result<()> {
    // Convert the origin to a treefile, and operate on that.
    // See https://github.com/coreos/rpm-ostree/issues/2326
    let tf = crate::origin::origin_to_treefile_inner(origin)?;
    let tf = &tf.parsed;

    // Package mappings.  Note these are inserted unconditionally, even if empty.
    vdict_insert_optvec(dict, "requested-packages", tf.packages.as_ref());
    vdict_insert_optvec(
        dict,
        "requested-modules",
        tf.modules.as_ref().map(|m| m.install.as_ref()).flatten(),
    );
    vdict_insert_optvec(
        dict,
        "modules-enabled",
        tf.modules.as_ref().map(|m| m.enable.as_ref()).flatten(),
    );
    vdict_insert_optmap(
        dict,
        "requested-local-packages",
        tf.derive.packages_local.as_ref(),
    );
    vdict_insert_optmap(
        dict,
        "requested-local-fileoverride-packages",
        tf.derive.packages_local_fileoverride.as_ref(),
    );
    vdict_insert_optvec(
        dict,
        "requested-base-removals",
        tf.derive.override_remove.as_ref(),
    );
    vdict_insert_optmap(
        dict,
        "requested-base-local-replacements",
        tf.derive.override_replace_local.as_ref(),
    );

    // Initramfs data.
    if let Some(initramfs) = tf.derive.initramfs.as_ref() {
        dict.insert("regenerate-initramfs", &initramfs.regenerate);
        vdict_insert_optvec(dict, "initramfs-args", initramfs.args.as_ref());
        vdict_insert_optvec(dict, "initramfs-etc", initramfs.etc.as_ref());
    } else {
        // This key is also always injected.
        dict.insert("regenerate-initramfs", &false);
    }

    // Other bits.
    if tf.cliwrap.unwrap_or_default() {
        dict.insert("cliwrap", &true);
    }

    Ok(())
}

/// Serialize information about the given deployment into the `dict`;
/// this will be exposed via DBus and is hence public API.
pub(crate) fn deployment_populate_variant(
    mut sysroot: Pin<&mut crate::FFIOstreeSysroot>,
    mut deployment: Pin<&mut crate::FFIOstreeDeployment>,
    mut dict: Pin<&mut crate::FFIGVariantDict>,
) -> CxxResult<()> {
    let sysroot = &sysroot.gobj_wrap();
    let deployment = &deployment.gobj_wrap();
    let dict = dict.gobj_wrap();

    let id = deployment_generate_id(deployment.gobj_rewrap());
    // First, basic values from ostree
    dict.insert("id", &id);

    dict.insert("osname", &deployment.osname().expect("osname").as_str());
    dict.insert("checksum", &deployment.csum().expect("csum").as_str());
    dict.insert_value("serial", &(deployment.deployserial() as i32).to_variant());

    let booted: bool = sysroot
        .booted_deployment()
        .map(|b| b.equal(deployment))
        .unwrap_or_default();
    dict.insert("booted", &booted);

    let live_state =
        crate::live::get_live_apply_state(sysroot.gobj_rewrap(), deployment.gobj_rewrap())?;
    if !live_state.inprogress.is_empty() {
        dict.insert("live-inprogress", &live_state.inprogress.as_str());
    }
    if !live_state.commit.is_empty() {
        dict.insert("live-replaced", &live_state.commit.as_str());
    }

    /* Staging status */
    dict.insert("staged", &deployment.is_staged());
    if deployment.is_staged()
        && std::path::Path::new("/run/ostree/staged-deployment-locked").exists()
    {
        dict.insert("finalization-locked", &true);
    }

    dict.insert("pinned", &deployment.is_pinned());
    let unlocked = deployment.unlocked();
    // Unwrap safety: This always returns a value
    dict.insert(
        "unlocked",
        &ostree::Deployment::unlocked_state_to_string(unlocked)
            .unwrap()
            .as_str(),
    );

    // Some of the origin-based state.  But not all yet; see the rest of the
    // code in rpmostreed-deployment-utils.cxx
    if let Some(origin) = deployment.origin() {
        deployment_populate_variant_origin(&origin, &dict)?;
    }

    Ok(())
}

/// Load basic layering metadata about a deployment commit.
pub(crate) fn deployment_layeredmeta_from_commit(
    mut deployment: Pin<&mut crate::FFIOstreeDeployment>,
    mut commit: Pin<&mut crate::FFIGVariant>,
) -> CxxResult<crate::ffi::DeploymentLayeredMeta> {
    let deployment = deployment.gobj_wrap();
    let commit = &commit.gobj_wrap();
    let metadata = &commit.child_value(0);
    let dict = &glib::VariantDict::new(Some(metadata));

    // More recent versions have an explicit clientlayer attribute (which
    // realistically will always be TRUE). For older versions, we just
    // rely on the treespec being present. */
    let is_layered = dict
        .lookup("rpmostree.clientlayer")
        .map_err(anyhow::Error::msg)?
        .unwrap_or_else(|| dict.contains("rpmostree.spec"));
    if !is_layered {
        Ok(crate::ffi::DeploymentLayeredMeta {
            is_layered,
            base_commit: deployment.csum().unwrap().into(),
            clientlayer_version: 0,
        })
    } else {
        let base_commit = ostree::commit_get_parent(commit)
            .expect("commit parent")
            .into();
        let clientlayer_version = dict
            .lookup_value("rpmostree.clientlayer_version", None)
            .map(|u| u.get::<u32>())
            .flatten()
            .unwrap_or_default();
        Ok(crate::ffi::DeploymentLayeredMeta {
            is_layered,
            base_commit,
            clientlayer_version,
        })
    }
}

/// Load basic layering metadata about a deployment
pub(crate) fn deployment_layeredmeta_load(
    mut repo: Pin<&mut crate::FFIOstreeRepo>,
    mut deployment: Pin<&mut crate::FFIOstreeDeployment>,
) -> CxxResult<crate::ffi::DeploymentLayeredMeta> {
    let repo = repo.gobj_wrap();
    let deployment = deployment.gobj_wrap();
    let commit = &repo.load_variant(
        ostree::ObjectType::Commit,
        deployment.csum().unwrap().as_str(),
    )?;
    deployment_layeredmeta_from_commit(deployment.gobj_rewrap(), commit.gobj_rewrap())
}

#[context("Loading origin status")]
pub(crate) fn variant_add_remote_status(
    mut repo: Pin<&mut crate::FFIOstreeRepo>,
    refspec: &str,
    base_checksum: &str,
    mut dict: Pin<&mut crate::FFIGVariantDict>,
) -> CxxResult<()> {
    let repo = &repo.gobj_wrap();
    let dict = &dict.gobj_wrap();

    let (maybe_remote, _) = ostree::parse_refspec(refspec)?;
    let gpg_verify = if let Some(ref remote) = maybe_remote {
        match repo.remote_get_gpg_verify(remote.as_str()) {
            Ok(b) => b,
            Err(err) => {
                if let Some(ioerr) = err.kind::<gio::IOErrorEnum>() {
                    // If the remote doesn't exist, let's note that so that status can render it
                    // specially.
                    if ioerr == gio::IOErrorEnum::NotFound {
                        let msg = format!("{}", err);
                        dict.insert("remote-error", &msg);
                        return Ok(());
                    }
                }
                return Err(err.into());
            }
        }
    } else {
        false
    };

    dict.insert("gpg-enabled", &gpg_verify);
    if !gpg_verify {
        return Ok(()); // note early return; no need to verify signatures!
    }

    // there must a remote if GPG checking is enabled
    let remote = maybe_remote.unwrap();

    match get_cached_signatures_variant(repo, remote.as_str(), base_checksum) {
        Ok(sigs) => dict.insert_value("signatures", &sigs),
        Err(err) => {
            // Somehow, we have a deployment which has gpg-verify=true, but we couldn't verify its
            // signature. Let's not just bomb out here. We need to return this in the variant so
            // that `status` can show the appropriate msg.
            tracing::debug!("failed to get cached signature variant: {}", err);
        }
    };

    Ok(())
}

fn get_cached_signatures_variant(
    repo: &ostree::Repo,
    remote: &str,
    checksum: &str,
) -> Result<glib::Variant> {
    let run = cap_std::fs::Dir::open_ambient_dir("/run", cap_std::ambient_authority())?;
    run.create_dir_all(RPM_OSTREED_COMMIT_VERIFICATION_CACHE)?;
    let cachedir = run.open_dir(RPM_OSTREED_COMMIT_VERIFICATION_CACHE)?;

    let remote_dir = Path::new(remote);
    let cached_relpath = remote_dir.join(checksum);

    if let Some(mut f) = cachedir.open_optional(&cached_relpath)? {
        let m = f.metadata()?;
        if m.mode() == (libc::S_IFREG | 0o600) && m.uid() == 0 && m.gid() == 0 {
            tracing::debug!("signature variant cache hit for ({}, {})", remote, checksum);
            let bytes = {
                let mut buf: Vec<u8> = Vec::new();
                f.read_to_end(&mut buf)?;
                glib::Bytes::from_owned(buf)
            };
            let v = glib::Variant::from_bytes::<Vec<glib::Variant>>(&bytes);
            return Ok(v);
        }
    }

    tracing::debug!(
        "signature variant cache miss for ({}, {})",
        remote,
        checksum
    );

    cachedir.create_dir_all(remote_dir)?;
    cachedir.remove_file_optional(&cached_relpath)?;
    let verify_result = repo.verify_commit_for_remote(checksum, remote, gio::NONE_CANCELLABLE)?;
    let n = verify_result.count_all();
    let mut sigs: Vec<glib::Variant> = Vec::with_capacity(n as usize);
    for i in 0..n {
        sigs.push(glib::Variant::from_variant(&verify_result.all(i).unwrap())); // we know index is in range
    }

    let v = glib::Variant::from_array::<glib::Variant>(&sigs);
    let perms = cap_std::fs::Permissions::from_mode(0o600);
    cachedir.replace_contents_with_perms(&cached_relpath, &v.data_as_bytes(), perms)?;
    Ok(v)
}

/// Parse kind and name for a source of package overrides.
#[context("Parsing override source '{}'", source)]
pub fn parse_override_source(source: &str) -> CxxResult<PackageOverrideSource> {
    let (kind_label, name) = source
        .split_once('=')
        .ok_or_else(|| format_err!("Not in KIND=NAME format"))?;

    let kind = match kind_label {
        "repo" => PackageOverrideSourceKind::Repo,
        x => return Err(anyhow!("Invalid kind '{}'", x).into()),
    };

    if name.is_empty() {
        return Err(anyhow!("Empty name").into());
    }

    Ok(PackageOverrideSource {
        kind,
        name: name.to_string(),
    })
}

/// Parse a revision (either a SHA256 checksum or a version metadata value).
///
/// This determines `revision` to either be a SHA256 checksum or a version
/// metadata value, and then returns the parsed value.
///
/// The input string may have a "revision=" prefix to denote a SHA256
/// checksum, or a "version=" prefix to denote a version metadata value.  If
/// the `revision` string lacks either prefix, the function attempts to infer
/// the type of revision. The prefixes are case-insensitive.
#[context("Parsing revision '{}'", revision)]
pub fn parse_revision(revision: &str) -> CxxResult<ParsedRevision> {
    // Check for an explicit prefix first.
    if let Some((key, value)) = revision.split_once('=') {
        let prefix = key.to_ascii_lowercase();
        if prefix == "revision" {
            // Since this claims to be a checksum, fail if it isn't.
            ostree::validate_checksum_string(value)?;

            return Ok(ParsedRevision {
                kind: ParsedRevisionKind::Checksum,
                value: value.to_string(),
            });
        } else if prefix == "version" {
            return Ok(ParsedRevision {
                kind: ParsedRevisionKind::Version,
                value: value.to_string(),
            });
        }
    };

    // If it looks like a checksum, assume it is.
    if ostree::validate_checksum_string(revision).is_ok() {
        return Ok(ParsedRevision {
            kind: ParsedRevisionKind::Checksum,
            value: revision.to_string(),
        });
    };

    // Treat anything else as a version metadata value.
    Ok(ParsedRevision {
        kind: ParsedRevisionKind::Version,
        value: revision.to_string(),
    })
}

#[cfg(test)]
mod test {
    use super::*;

    fn origin_to_variant(s: &str) -> glib::VariantDict {
        let kf = &crate::origin::test::kf_from_str(s).unwrap();
        let vdict = glib::VariantDict::new(None);
        deployment_populate_variant_origin(kf, &vdict).unwrap();
        vdict
    }

    #[test]
    fn origin_variant() {
        let vdict = &origin_to_variant(crate::origin::test::COMPLEX);
        // Operating on container-typed variants in current glib 0.10 + Rust is painful.  Just validate
        // that a value exists as a start.  It's *much* better in glib 0.14, but
        // porting to that is a ways away.
        assert!(vdict.lookup_value("requested-packages", None).is_some());
    }

    #[test]
    fn test_parse_override_source() {
        let ok_cases = [("repo=custom", (PackageOverrideSourceKind::Repo, "custom"))];
        for (input, expected) in ok_cases {
            let out = parse_override_source(input).unwrap();
            assert_eq!(out.kind, expected.0);
            assert_eq!(out.name, expected.1);
        }

        let err_cases = ["", "repo", "repo=", "foo=bar"];
        for input in err_cases {
            parse_override_source(input).unwrap_err();
        }
    }

    #[test]
    fn test_parse_revision() {
        let ok_cases = [
            (
                "revision=078eea7e710630c0c8aec1071d3529fc307d8772f2ca3435fdc947a39c5a12bf",
                (
                    ParsedRevisionKind::Checksum,
                    "078eea7e710630c0c8aec1071d3529fc307d8772f2ca3435fdc947a39c5a12bf",
                ),
            ),
            (
                "078eea7e710630c0c8aec1071d3529fc307d8772f2ca3435fdc947a39c5a12bf",
                (
                    ParsedRevisionKind::Checksum,
                    "078eea7e710630c0c8aec1071d3529fc307d8772f2ca3435fdc947a39c5a12bf",
                ),
            ),
            (
                "version=078eea7e710630c0c8aec1071d3529fc307d8772f2ca3435fdc947a39c5a12bf",
                (
                    ParsedRevisionKind::Version,
                    "078eea7e710630c0c8aec1071d3529fc307d8772f2ca3435fdc947a39c5a12bf",
                ),
            ),
            ("version=foo", (ParsedRevisionKind::Version, "foo")),
            ("version=", (ParsedRevisionKind::Version, "")),
            ("foo", (ParsedRevisionKind::Version, "foo")),
            ("", (ParsedRevisionKind::Version, "")),
        ];
        for (input, expected) in ok_cases {
            let out = parse_revision(input).unwrap();
            assert_eq!(out.kind, expected.0);
            assert_eq!(out.value, expected.1);
        }

        let err_cases = ["revision=", "revision=bar"];
        for input in err_cases {
            parse_override_source(input).unwrap_err();
        }
    }
}
