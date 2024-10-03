//! Logic for server-side builds; corresponds to rpmostree-builtin-compose-tree.cxx

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::fs::File;
use std::io::{BufWriter, Write};
use std::process::Command;

use anyhow::{anyhow, Result};
use camino::{Utf8Path, Utf8PathBuf};
use clap::Parser;
use oci_spec::image::ImageManifest;
use ostree::gio;
use ostree_ext::container as ostree_container;
use ostree_ext::containers_image_proxy;
use ostree_ext::keyfileext::{map_keyfile_optional, KeyFileExt};
use ostree_ext::{oci_spec, ostree};

use crate::cxxrsutil::{CxxResult, FFIGObjectWrapper};

#[derive(clap::ValueEnum, Clone, Debug)]
enum OutputFormat {
    Ociarchive,
    Oci,
    Registry,
}

impl Default for OutputFormat {
    fn default() -> Self {
        Self::Ociarchive
    }
}

impl Into<ostree_container::Transport> for OutputFormat {
    fn into(self) -> ostree_container::Transport {
        match self {
            OutputFormat::Ociarchive => ostree_container::Transport::OciArchive,
            OutputFormat::Oci => ostree_container::Transport::OciDir,
            OutputFormat::Registry => ostree_container::Transport::Registry,
        }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq, PartialOrd, Ord, clap::ValueEnum)]
enum InitializeMode {
    /// Require the image to already exist.  For backwards compatibility reasons, this is the default.
    Query,
    /// Always overwrite the target image, even if it already exists and there were no changes.
    Always,
    /// Error out if the target image does not already exist.
    Never,
    /// Initialize if the target image does not already exist.
    IfNotExists,
}

impl std::fmt::Display for InitializeMode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            InitializeMode::Query => "query",
            InitializeMode::Always => "always",
            InitializeMode::Never => "never",
            InitializeMode::IfNotExists => "if-not-exists",
        };
        f.write_str(s)
    }
}

impl Default for InitializeMode {
    fn default() -> Self {
        Self::Query
    }
}

#[derive(Debug, Parser)]
struct Opt {
    #[clap(long)]
    #[clap(value_parser)]
    /// Directory to use for caching downloaded packages and other data
    cachedir: Option<Utf8PathBuf>,

    /// Container authentication file
    #[clap(long)]
    #[clap(value_parser)]
    authfile: Option<Utf8PathBuf>,

    /// OSTree repository to use for `ostree-layers` and `ostree-override-layers`
    #[clap(long)]
    #[clap(value_parser)]
    layer_repo: Option<Utf8PathBuf>,

    #[clap(long, short = 'i', conflicts_with = "initialize_mode")]
    /// Do not query previous image in target location; use this for the first build
    initialize: bool,

    /// Control conditions under which the image is written
    #[clap(long, conflicts_with = "initialize", default_value_t)]
    initialize_mode: InitializeMode,

    #[clap(long, value_enum, default_value_t)]
    format: OutputFormat,

    #[clap(long)]
    /// Force a build
    force_nocache: bool,

    #[clap(long)]
    /// Operate only on cached data, do not access network repositories
    offline: bool,

    #[clap(long = "lockfile", value_parser)]
    /// JSON-formatted lockfile; can be specified multiple times.
    lockfiles: Vec<Utf8PathBuf>,

    /// Additional labels for the container image, in KEY=VALUE format
    #[clap(name = "label", long, short)]
    labels: Vec<String>,

    /// Path to container image configuration in JSON format.  This is the `config`
    /// field of https://github.com/opencontainers/image-spec/blob/main/config.md
    #[clap(long)]
    image_config: Option<Utf8PathBuf>,

    #[clap(long, value_parser)]
    /// Update the timestamp or create this file on changes
    touch_if_changed: Option<Utf8PathBuf>,

    #[clap(long)]
    /// Number of times to retry copying an image to remote destination (e.g. registry)
    copy_retry_times: Option<u32>,

    #[clap(value_parser)]
    /// Path to the manifest file
    manifest: Utf8PathBuf,

    #[clap(value_parser)]
    /// Target path to write
    output: Utf8PathBuf,
}

/// Metadata relevant to base image builds that we extract from the container metadata.
struct ImageMetadata {
    manifest: ImageManifest,
    version: Option<String>,
    inputhash: String,
}

/// Fetch the previous metadata from the container image metadata.
async fn fetch_previous_metadata(
    proxy: &containers_image_proxy::ImageProxy,
    oi: &containers_image_proxy::OpenedImage,
) -> Result<ImageMetadata> {
    let manifest = proxy.fetch_manifest(oi).await?.1;
    let config = proxy.fetch_config(oi).await?;
    const INPUTHASH_KEY: &str = "rpmostree.inputhash";
    let labels = config
        .config()
        .as_ref()
        .ok_or_else(|| anyhow!("Missing config"))?
        .labels()
        .as_ref()
        .ok_or_else(|| anyhow!("Missing config labels"))?;

    let inputhash = labels
        .get(INPUTHASH_KEY)
        .ok_or_else(|| anyhow!("Missing config label {INPUTHASH_KEY}"))?
        .to_owned();
    let version = ostree_container::version_for_config(&config).map(ToOwned::to_owned);
    Ok(ImageMetadata {
        manifest,
        version,
        inputhash,
    })
}

pub(crate) fn compose_image(args: Vec<String>) -> CxxResult<()> {
    use crate::isolation::self_command;
    let cancellable = gio::Cancellable::NONE;

    let opt = Opt::parse_from(args.iter().skip(1));

    let tempdir = tempfile::tempdir()?;
    let tempdir = Utf8Path::from_path(tempdir.path()).unwrap();

    let handle = tokio::runtime::Handle::current();
    let proxy = handle
        .block_on(async {
            let config = containers_image_proxy::ImageProxyConfig {
                authfile: opt.authfile.as_ref().map(|v| v.as_std_path().to_owned()),
                ..Default::default()
            };
            containers_image_proxy::ImageProxy::new_with_config(config).await
        })
        .expect("Create an image proxy");

    let (_cachetempdir, cachedir) = match opt.cachedir {
        Some(p) => (None, p),
        None => {
            let t = tempfile::tempdir_in("/var/tmp")?;
            let p = Utf8Path::from_path(t.path()).unwrap().to_owned();
            (Some(t), p)
        }
    };
    let cachedir = &cachedir;
    if !cachedir.try_exists()? {
        return Err(format!("Failed to find specified cachedir: {cachedir}").into());
    }
    let treecachedir = cachedir.join("v0");
    if !treecachedir.exists() {
        std::fs::create_dir(&treecachedir)?;
    }
    let repo = cachedir.join("repo");
    if !repo.exists() {
        let _repo = ostree::Repo::create_at(
            libc::AT_FDCWD,
            repo.as_str(),
            ostree::RepoMode::BareUser,
            None,
            cancellable,
        )?;
    }

    let target_imgref = ostree_container::ImageReference {
        transport: opt.format.clone().into(),
        name: opt.output.to_string(),
    };
    let previous_meta = if opt.initialize || matches!(opt.initialize_mode, InitializeMode::Always) {
        None
    } else {
        assert!(!opt.initialize); // Checked by clap
        let handle = tokio::runtime::Handle::current();
        handle.block_on(async {
            let oi = if matches!(opt.initialize_mode, InitializeMode::Query) {
                // In the default query mode, we error if the image doesn't exist, so this always
                // gets mapped to Some().
                Some(proxy.open_image(&target_imgref.to_string()).await?)
            } else {
                // All other cases check the Option.
                proxy
                    .open_image_optional(&target_imgref.to_string())
                    .await?
            };
            let meta = match (opt.initialize_mode, oi.as_ref()) {
                (InitializeMode::Always, _) => unreachable!(), // Handled above
                (InitializeMode::Query, None) => unreachable!(), // Handled above
                (InitializeMode::Never, Some(_)) => anyhow::bail!("Target image already exists"),
                (InitializeMode::IfNotExists | InitializeMode::Never, None) => None,
                (InitializeMode::IfNotExists | InitializeMode::Query, Some(oi)) => {
                    Some(fetch_previous_metadata(&proxy, oi).await?)
                }
            };
            anyhow::Ok(meta)
        })?
    };
    let mut compose_args_extra = Vec::new();
    if let Some(m) = previous_meta.as_ref() {
        compose_args_extra.extend(["--previous-inputhash", m.inputhash.as_str()]);
        if let Some(v) = m.version.as_ref() {
            compose_args_extra.extend(["--previous-version", v])
        }
    }
    if let Some(layer_repo) = opt.layer_repo.as_deref() {
        compose_args_extra.extend(["--layer-repo", layer_repo.as_str()]);
    }

    for lockfile in opt.lockfiles.iter() {
        compose_args_extra.extend(["--ex-lockfile", lockfile.as_str()]);
    }

    let commitid_path = tempdir.join("commitid");
    let changed_path = tempdir.join("changed");

    let s = self_command()
        .args(&[
            "compose",
            "tree",
            "--unified-core",
            "--repo",
            repo.as_str(),
            "--write-commitid-to",
            commitid_path.as_str(),
            "--touch-if-changed",
            changed_path.as_str(),
            "--cachedir",
            treecachedir.as_str(),
        ])
        .args(opt.force_nocache.then(|| "--force-nocache"))
        .args(opt.offline.then(|| "--cache-only"))
        .args(compose_args_extra)
        .arg(opt.manifest.as_str())
        .status()?;
    if !s.success() {
        return Err(anyhow::anyhow!("compose tree failed: {:?}", s).into());
    }

    if !changed_path.exists() {
        return Ok(());
    }

    if let Some(p) = opt.touch_if_changed.as_ref() {
        std::fs::write(p, "")?;
    }

    let commitid = std::fs::read_to_string(&commitid_path)?;
    let tempdest = match target_imgref.transport {
        ostree_container::Transport::Registry => Some(
            ostree_container::ImageReference {
                transport: ostree_container::Transport::OciDir,
                name: tempdir.join("tempdest").to_string(),
            }
            .to_string(),
        ),
        _ => None,
    };
    let target_imgref = target_imgref.to_string();

    let label_args = opt.labels.into_iter().map(|v| format!("--label={v}"));
    // If we have a prior build, pass its manifest to the encapsulation command to allow reuse of packing structure.
    let previous_arg = previous_meta
        .as_ref()
        .map(|previous_meta| {
            let manifest_path = tempdir.join("previous-manifest.json");
            let mut f = File::create(&manifest_path).map(BufWriter::new)?;
            serde_json::to_writer(&mut f, &previous_meta.manifest).map_err(anyhow::Error::new)?;
            f.flush()?;
            anyhow::Ok(format!("--previous-build-manifest={manifest_path}"))
        })
        .transpose()?;

    let s = self_command()
        .args(&["compose", "container-encapsulate"])
        .args(label_args)
        .args(previous_arg)
        .args(opt.image_config.map(|v| format!("--image-config={v}")))
        .args(&[
            "--repo",
            repo.as_str(),
            commitid.as_str(),
            tempdest
                .as_ref()
                .map(|s| s.as_str())
                .unwrap_or_else(|| target_imgref.as_str()),
        ])
        .status()?;
    if !s.success() {
        return Err(anyhow::anyhow!("container-encapsulate failed: {:?}", s).into());
    }

    if let Some(tempdest) = tempdest {
        let mut c = Command::new("skopeo");
        c.arg("copy");
        if let Some(authfile) = opt.authfile.as_ref() {
            c.args(["--authfile", authfile.as_str()]);
        }
        let retry_times = opt.copy_retry_times.unwrap_or_default();
        if retry_times > 0 {
            c.arg(format!("--retry-times={retry_times}"));
        }
        c.args([tempdest.as_str(), target_imgref.as_str()]);
        let status = c.status()?;
        if !status.success() {
            return Err(format!("Failed to run skopeo copy: {status:?}").into());
        }
    }

    if let Some(previous_meta) = previous_meta {
        let new_manifest = handle.block_on(async {
            let oi = &proxy.open_image(&target_imgref).await?;
            let manifest = proxy.fetch_manifest(oi).await?.1;
            Ok::<_, anyhow::Error>(manifest)
        })?;

        let diff = ostree_ext::container::ManifestDiff::new(&previous_meta.manifest, &new_manifest);
        diff.print();
    }

    println!("Wrote: {target_imgref}");

    Ok(())
}

/// Set up a cache/build repository using configuration present in the target repository.
pub(crate) fn configure_build_repo_from_target(
    build_repo: &crate::FFIOstreeRepo,
    target_repo: &crate::FFIOstreeRepo,
) -> CxxResult<()> {
    // If we're not fsyncing the target, don't fsync the build repo either.  We also
    // want to have the same fsverity/composefs flags.
    let propagated_bools = [("core", "fsync")].iter();
    // We entirely copy these groups.  See https://github.com/ostreedev/ostree/pull/2640 the initial
    // creation of the ex-integrity group.
    let propagated_groups = ["ex-integrity"].iter();
    let build_repo = &build_repo.glib_reborrow();
    let target_repo = &target_repo.glib_reborrow();

    let mut changed = false;
    let build_config = build_repo.config();
    let target_config = target_repo.copy_config();
    for (group, key) in propagated_bools {
        if let Some(v) = target_config.optional_bool(group, key)? {
            changed = true;
            tracing::debug!("Propagating {group}.{key} with value {v}");
            build_config.set_boolean(group, key, v);
        }
    }
    for group in propagated_groups {
        for key in map_keyfile_optional(target_config.keys(group))?
            .iter()
            .flatten()
        {
            let key = key.to_str();
            changed = true;
            let value = target_config.value(group, key)?;
            tracing::debug!("Propagating {group}.{key} with value {value}");
            build_config.set_value(group, key, &value);
        }
    }

    if changed {
        build_repo.write_config(&build_config)?;
    }

    Ok(())
}
