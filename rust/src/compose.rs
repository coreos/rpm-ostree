//! Logic for server-side builds; corresponds to rpmostree-builtin-compose-tree.cxx

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::ffi::OsStr;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::os::fd::{AsFd, AsRawFd};
use std::process::{Command, Stdio};

use anyhow::{anyhow, Context, Result};
use camino::{Utf8Path, Utf8PathBuf};
use cap_std::fs::{Dir, MetadataExt};
use clap::Parser;
use fn_error_context::context;
use oci_spec::image::ImageManifest;
use ostree::gio;
use ostree_ext::containers_image_proxy;
use ostree_ext::glib::{Cast, ToVariant};
use ostree_ext::keyfileext::{map_keyfile_optional, KeyFileExt};
use ostree_ext::ostree::MutableTree;
use ostree_ext::{container as ostree_container, glib};
use ostree_ext::{oci_spec, ostree};

use crate::cmdutils::CommandRunExt;
use crate::cxxrsutil::{CxxResult, FFIGObjectWrapper};

const SYSROOT: &str = "sysroot";
const USR: &str = "usr";
const ETC: &str = "etc";
const USR_ETC: &str = "usr/etc";

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

impl From<OutputFormat> for ostree_container::Transport {
    fn from(val: OutputFormat) -> Self {
        match val {
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

    #[clap(long)]
    #[clap(value_parser)]
    /// Rootfs to use for resolving releasever if unset
    source_root: Option<Utf8PathBuf>,

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

/// Generate a "chunked" OCI archive from an input rootfs.
#[derive(Debug, Parser)]
pub(crate) struct BuildChunkedOCI {
    /// Path to the source root filesystem tree.
    #[clap(long, required_unless_present = "from")]
    rootfs: Option<Utf8PathBuf>,

    /// Use the provided image (in containers-storage).
    #[clap(long, required_unless_present = "rootfs")]
    from: Option<String>,

    /// If set, configure the output OCI image to be a bootc container.
    /// At the current time this option is required.
    #[clap(long, required = true)]
    bootc: bool,

    /// The format version. At the current time there is only version `1`.
    /// It is required to pass this option in order to enable future
    /// format evolution.
    #[clap(long, required = true)]
    format_version: u32,

    /// Tag to use for output image, or `latest` if unset.
    #[clap(long, default_value = "latest")]
    reference: String,

    /// Output image reference, in TRANSPORT:TARGET syntax.
    /// For example, `containers-storage:localhost/exampleos` or `oci:/path/to/ocidir`.
    #[clap(long, required = true)]
    output: String,
}

struct PodmanMount {
    path: Utf8PathBuf,
    temp_cid: Option<String>,
    mounted: bool,
}

impl PodmanMount {
    #[context("Unmounting container")]
    fn _impl_unmount(&mut self) -> Result<()> {
        if self.mounted {
            tracing::debug!("unmounting {}", self.path.as_str());
            self.mounted = false;
            Command::new("umount")
                .args(["-l", self.path.as_str()])
                .stdout(Stdio::null())
                .run()
                .context("umount")?;
            tracing::trace!("umount ok");
        }
        if let Some(cid) = self.temp_cid.take() {
            tracing::debug!("rm container {cid}");
            Command::new("podman")
                .args(["rm", cid.as_str()])
                .stdout(Stdio::null())
                .run()
                .context("podman rm")?;
            tracing::trace!("rm ok");
        }
        Ok(())
    }

    #[context("Mounting continer {container}")]
    fn _impl_mount(container: &str) -> Result<Utf8PathBuf> {
        let mut o = Command::new("podman")
            .args(["mount", container])
            .run_get_output()?;
        let mut s = String::new();
        o.read_to_string(&mut s)?;
        while s.ends_with('\n') {
            s.pop();
        }
        tracing::debug!("mounted container {container} at {s}");
        Ok(s.into())
    }

    #[allow(dead_code)]
    fn new_for_container(container: &str) -> Result<Self> {
        let path = Self::_impl_mount(container)?;
        Ok(Self {
            path,
            temp_cid: None,
            mounted: true,
        })
    }

    fn new_for_image(image: &str) -> Result<Self> {
        let mut o = Command::new("podman")
            .args(["create", image])
            .run_get_output()?;
        let mut s = String::new();
        o.read_to_string(&mut s)?;
        let cid = s.trim();
        let path = Self::_impl_mount(cid)?;
        tracing::debug!("created container {cid} from {image}");
        Ok(Self {
            path,
            temp_cid: Some(cid.to_owned()),
            mounted: true,
        })
    }

    fn unmount(mut self) -> Result<()> {
        self._impl_unmount()
    }
}

impl Drop for PodmanMount {
    fn drop(&mut self) {
        tracing::trace!("In drop, mounted={}", self.mounted);
        let _ = self._impl_unmount();
    }
}

impl BuildChunkedOCI {
    pub(crate) fn run(self) -> Result<()> {
        enum FileSource {
            Rootfs(Utf8PathBuf),
            Podman(PodmanMount),
        }
        let rootfs_source = if let Some(rootfs) = self.rootfs {
            FileSource::Rootfs(rootfs)
        } else {
            let image = self.from.as_deref().unwrap();
            FileSource::Podman(PodmanMount::new_for_image(image)?)
        };
        let rootfs = match &rootfs_source {
            FileSource::Rootfs(p) => p.as_path(),
            FileSource::Podman(mnt) => mnt.path.as_path(),
        };
        let rootfs = Dir::open_ambient_dir(rootfs, cap_std::ambient_authority())
            .with_context(|| format!("Opening {}", rootfs))?;
        // These must be set to exactly this; the CLI parser requires it.
        assert!(self.bootc);
        assert_eq!(self.format_version, 1);

        let td = tempfile::tempdir_in("/var/tmp")?;

        // Note: In a format v2, we'd likely not use ostree.
        let repo_path: Utf8PathBuf = td.path().join("repo").try_into()?;
        let repo = ostree::Repo::create_at(
            libc::AT_FDCWD,
            repo_path.as_str(),
            ostree::RepoMode::BareUser,
            None,
            gio::Cancellable::NONE,
        )?;

        println!("Generating commit...");
        // It's only the tests that override
        let modifier =
            ostree::RepoCommitModifier::new(ostree::RepoCommitModifierFlags::empty(), None);
        let commitid = generate_commit_from_rootfs(&repo, &rootfs, modifier)?;

        let label_arg = self
            .bootc
            .then_some(["--label", "containers.bootc=1"].as_slice())
            .unwrap_or_default();
        let config_data = if let Some(image) = self.from.as_deref() {
            let img_transport = format!("containers-storage:{image}");
            let tmpf = tempfile::NamedTempFile::new()?;
            Command::new("skopeo")
                .args(["inspect", "--config", img_transport.as_str()])
                .stdout(tmpf.as_file().try_clone()?)
                .run()?;
            Some(tmpf.into_temp_path())
        } else {
            None
        };
        crate::isolation::self_command()
            .args([
                "compose",
                "container-encapsulate",
                "--repo",
                repo_path.as_str(),
            ])
            .args(label_arg)
            .args(
                config_data
                    .iter()
                    .flat_map(|c| [OsStr::new("--image-config"), c.as_os_str()]),
            )
            .args([commitid.as_str(), self.output.as_str()])
            .run()
            .context("Invoking compose container-encapsulate")?;

        drop(rootfs);
        match rootfs_source {
            FileSource::Rootfs(_) => {}
            FileSource::Podman(mnt) => {
                mnt.unmount().context("Final mount cleanup")?;
            }
        }

        Ok(())
    }
}

fn label_to_xattrs(label: Option<&str>) -> Option<glib::Variant> {
    let xattrs = label.map(|label| {
        let mut label: Vec<_> = label.to_owned().into();
        label.push(0);
        vec![(c"security.selinux".to_bytes_with_nul(), label)]
    });
    xattrs.map(|x| x.to_variant())
}

fn create_root_dirmeta(root: &Dir, policy: &ostree::SePolicy) -> Result<glib::Variant> {
    let finfo = gio::FileInfo::new();
    let meta = root.dir_metadata()?;
    finfo.set_attribute_uint32("unix::uid", 0);
    finfo.set_attribute_uint32("unix::gid", 0);
    finfo.set_attribute_uint32("unix::mode", libc::S_IFDIR | meta.mode());
    let label = policy.label("/", 0o777 | libc::S_IFDIR, gio::Cancellable::NONE)?;
    let xattrs = label_to_xattrs(label.as_deref());
    let r = ostree::create_directory_metadata(&finfo, xattrs.as_ref());
    Ok(r)
}

enum MtreeEntry {
    #[allow(dead_code)]
    Leaf(String),
    Directory(MutableTree),
}

impl MtreeEntry {
    fn require_dir(self) -> Result<MutableTree> {
        match self {
            MtreeEntry::Leaf(_) => anyhow::bail!("Expected a directory"),
            MtreeEntry::Directory(t) => Ok(t),
        }
    }
}

/// The two returns value in C are mutually exclusive; also map "not found" to None.
fn mtree_lookup(t: &ostree::MutableTree, path: &str) -> Result<Option<MtreeEntry>> {
    let r = match t.lookup(path) {
        Ok((Some(leaf), None)) => Some(MtreeEntry::Leaf(leaf.into())),
        Ok((_, Some(subdir))) => Some(MtreeEntry::Directory(subdir)),
        Ok((None, None)) => unreachable!(),
        Err(e) if e.matches(gio::IOErrorEnum::NotFound) => None,
        Err(e) => return Err(e.into()),
    };
    Ok(r)
}

// Given a root filesystem, perform some in-memory postprocessing.
// At the moment, that's just ensuring /etc is /usr/etc.
#[context("Postprocessing commit")]
fn postprocess_mtree(repo: &ostree::Repo, rootfs: &ostree::MutableTree) -> Result<()> {
    let etc_subdir = mtree_lookup(rootfs, ETC)?
        .map(|e| e.require_dir().context("/etc"))
        .transpose()?;
    let usr_etc_subdir = mtree_lookup(rootfs, USR_ETC)?
        .map(|e| e.require_dir().context("/usr/etc"))
        .transpose()?;
    match (etc_subdir, usr_etc_subdir) {
        (None, None) => {
            // No /etc? We'll let you try it.
        }
        (None, Some(_)) => {
            // Having just /usr/etc is the expected ostree default.
        }
        (Some(etc), None) => {
            // We need to write the etc dir now to generate checksums,
            // then move it.
            repo.write_mtree(&etc, gio::Cancellable::NONE)?;
            let usr = rootfs
                .lookup(USR)?
                .1
                .ok_or_else(|| anyhow!("Missing /usr"))?;
            let usretc = usr.ensure_dir(ETC)?;
            usretc.set_contents_checksum(&etc.contents_checksum());
            usretc.set_metadata_checksum(&etc.metadata_checksum());
            rootfs.remove(ETC, false)?;
        }
        (Some(_), Some(_)) => {
            anyhow::bail!("Found both /etc and /usr/etc");
        }
    }
    Ok(())
}

fn generate_commit_from_rootfs(
    repo: &ostree::Repo,
    rootfs: &Dir,
    modifier: ostree::RepoCommitModifier,
) -> Result<String> {
    let root_mtree = ostree::MutableTree::new();
    let cancellable = gio::Cancellable::NONE;
    let tx = repo.auto_transaction(cancellable)?;

    let policy = ostree::SePolicy::new_at(rootfs.as_fd().as_raw_fd(), cancellable)?;
    modifier.set_sepolicy(Some(&policy));

    let root_dirmeta = create_root_dirmeta(rootfs, &policy)?;
    let root_metachecksum = repo.write_metadata(
        ostree::ObjectType::DirMeta,
        None,
        &root_dirmeta,
        cancellable,
    )?;
    root_mtree.set_metadata_checksum(&root_metachecksum.to_hex());

    for ent in rootfs.entries_utf8()? {
        let ent = ent?;
        let name = ent.file_name()?;

        let ftype = ent.file_type()?;
        // Skip the contents of the sysroot
        if ftype.is_dir() && name == SYSROOT {
            let child_mtree = root_mtree.ensure_dir(&name)?;
            child_mtree.set_metadata_checksum(&root_metachecksum.to_hex());
        } else if ftype.is_dir() {
            let child_mtree = root_mtree.ensure_dir(&name)?;
            let child = ent.open_dir()?;
            repo.write_dfd_to_mtree(
                child.as_raw_fd(),
                ".",
                &child_mtree,
                Some(&modifier),
                cancellable,
            )?;
        } else if ftype.is_symlink() {
            let contents: Utf8PathBuf = rootfs
                .read_link_contents(&name)
                .with_context(|| format!("Reading {name}"))?
                .try_into()?;
            // Label lookups need to be absolute
            let selabel_path = format!("/{name}");
            let label = policy.label(selabel_path.as_str(), 0o777 | libc::S_IFLNK, cancellable)?;
            let xattrs = label_to_xattrs(label.as_deref());
            let link_checksum =
                repo.write_symlink(None, 0, 0, xattrs.as_ref(), contents.as_str(), cancellable)?;
            root_mtree.replace_file(&name, &link_checksum)?;
        } else {
            // Yes we could support this but it's a surprising amount of typing
            anyhow::bail!("Unsupported regular file {name} at toplevel");
        }
    }

    postprocess_mtree(repo, &root_mtree)?;

    let ostree_root = repo.write_mtree(&root_mtree, cancellable)?;
    let ostree_root = ostree_root.downcast_ref::<ostree::RepoFile>().unwrap();
    let commit =
        repo.write_commit_with_time(None, None, None, None, ostree_root, 0, cancellable)?;

    tx.commit(cancellable)?;
    Ok(commit.into())
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

    if let Some(ref path) = opt.source_root {
        compose_args_extra.extend(["--source-root", path.as_str()]);
    }

    self_command()
        .args([
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
        .args(opt.force_nocache.then_some("--force-nocache"))
        .args(opt.offline.then_some("--cache-only"))
        .args(compose_args_extra)
        .arg(opt.manifest.as_str())
        .run()?;

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

    self_command()
        .args(["compose", "container-encapsulate"])
        .args(label_args)
        .args(previous_arg)
        .args(opt.image_config.map(|v| format!("--image-config={v}")))
        .args([
            "--repo",
            repo.as_str(),
            commitid.as_str(),
            tempdest.as_deref().unwrap_or(target_imgref.as_str()),
        ])
        .run()?;

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
            let key = key.as_str();
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

#[cfg(test)]
mod tests {
    use cap_std::fs::PermissionsExt;
    use cap_std_ext::cap_tempfile;
    use gio::prelude::FileExt;

    use super::*;

    fn commit_filter(
        _repo: &ostree::Repo,
        _name: &str,
        info: &gio::FileInfo,
    ) -> ostree::RepoCommitFilterResult {
        info.set_attribute_uint32("unix::uid", 0);
        info.set_attribute_uint32("unix::gid", 0);
        ostree::RepoCommitFilterResult::Allow
    }

    #[test]
    fn write_commit() -> Result<()> {
        let cancellable = gio::Cancellable::NONE;
        let repo_td = cap_tempfile::tempdir(cap_std::ambient_authority())?;
        let repo =
            ostree::Repo::create_at_dir(repo_td.as_fd(), ".", ostree::RepoMode::BareUser, None)?;
        let base_td = cap_tempfile::tempdir(cap_std::ambient_authority())?;
        base_td.create_dir("root")?;
        // Ensure the base permissions are predictable
        let td = base_td.open_dir("root")?;
        td.set_permissions(".", cap_std::fs::Permissions::from_mode(0o755))?;

        let modifier = ostree::RepoCommitModifier::new(
            ostree::RepoCommitModifierFlags::SKIP_XATTRS
                | ostree::RepoCommitModifierFlags::CANONICAL_PERMISSIONS,
            Some(Box::new(commit_filter)),
        );

        let commit = generate_commit_from_rootfs(&repo, &td, modifier.clone()).unwrap();
        // Verify there are zero children
        let commit_root = repo.read_commit(&commit, cancellable)?.0;
        {
            let e = commit_root.enumerate_children(
                "standard::*",
                gio::FileQueryInfoFlags::NOFOLLOW_SYMLINKS,
                cancellable,
            )?;
            assert_eq!(e.into_iter().count(), 0);
        }
        // Verify the contents checksum
        let commit_obj = repo.load_commit(&commit)?.0;
        let contents_checksum = ostree::commit_get_content_checksum(&commit_obj).unwrap();
        assert_eq!(
            contents_checksum,
            "978b7746df27a18398d9099592a905091a496bf53f9158c1f350d1d410424f66"
        );
        // And because we use a predictable timestamp, the commit should be predictable
        assert_eq!(
            commit,
            "6109d33e99f48e9b90cdf8ad037b8e5d20ef899697cfd3eb492cf78800aed498"
        );

        td.create_dir_all("usr/bin")?;
        td.write("usr/bin/bash", "bash binary")?;
        td.create_dir("etc")?;
        td.write("etc/foo", "some etc content")?;
        td.create_dir_all("sysroot/ostree/repo/objects/00")?;
        td.write(
            "sysroot/ostree/repo/objects/00/foo.commit",
            "commit to ignore",
        )?;

        let commit = generate_commit_from_rootfs(&repo, &td, modifier.clone()).unwrap();
        assert_eq!(
            commit,
            "192642d885cd1f0a743455466bb918415f004c2af6b69e87e00768623ad7fa04"
        );

        let ostree_root = repo.read_commit(&commit, cancellable)?.0;

        let foo = ostree_root.resolve_relative_path("usr/etc/foo");
        assert!(foo.query_exists(cancellable));
        let meta = foo.query_info(
            "standard::*",
            gio::FileQueryInfoFlags::NOFOLLOW_SYMLINKS,
            cancellable,
        )?;
        assert_eq!(meta.size(), "some etc content".len() as i64);
        assert!(!ostree_root
            .resolve_relative_path("etc")
            .query_exists(cancellable));

        let ostree_top = ostree_root.resolve_relative_path("sysroot");
        let meta = ostree_top.query_info(
            "standard::*",
            gio::FileQueryInfoFlags::NOFOLLOW_SYMLINKS,
            cancellable,
        )?;
        assert_eq!(meta.file_type(), gio::FileType::Directory);
        assert!(!ostree_root
            .resolve_relative_path("sysroot/ostree")
            .query_exists(cancellable));

        let bash_path = ostree_root.resolve_relative_path("usr/bin/bash");
        let bash_path = bash_path.downcast_ref::<ostree::RepoFile>().unwrap();
        bash_path.ensure_resolved()?;
        let bashmeta = bash_path.query_info(
            "standard::*",
            gio::FileQueryInfoFlags::NOFOLLOW_SYMLINKS,
            cancellable,
        )?;
        assert_eq!(bashmeta.size(), 11);

        Ok(())
    }
}
