//! Logic for server-side builds; corresponds to rpmostree-builtin-compose-tree.cxx

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::borrow::Cow;
use std::collections::BTreeSet;
use std::ffi::{OsStr, OsString};
use std::fs::File;
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::num::NonZeroU32;
use std::os::fd::{AsFd, AsRawFd};
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{anyhow, Context, Result};
use camino::{Utf8Path, Utf8PathBuf};
use cap_std::fs::{Dir, MetadataExt};
use cap_std_ext::dirext::CapStdExtDirExt;
use clap::Parser;
use fn_error_context::context;
use oci_spec::image::ImageManifest;
use ostree::gio;
use ostree_ext::containers_image_proxy;
use ostree_ext::glib::prelude::*;
use ostree_ext::keyfileext::{map_keyfile_optional, KeyFileExt};
use ostree_ext::oci_spec::image::ImageConfiguration;
use ostree_ext::ostree::MutableTree;
use ostree_ext::{container as ostree_container, glib};
use ostree_ext::{oci_spec, ostree};

use crate::cmdutils::CommandRunExt;
use crate::containers_storage::Mount;
use crate::cxxrsutil::{CxxResult, FFIGObjectWrapper};
use crate::isolation::self_command;
use crate::{RPMOSTREE_RPMDB_LOCATION, RPMOSTREE_SYSIMAGE_RPMDB};

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
struct ComposeImageOpts {
    #[clap(long)]
    #[clap(value_parser)]
    /// Directory to use for caching downloaded packages and other data
    cachedir: Option<Utf8PathBuf>,

    #[clap(long)]
    #[clap(value_parser)]
    /// Rootfs to use for resolving package system configuration, such
    /// as the yum repository configuration, releasever, etc.
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
pub(crate) struct BuildChunkedOCIOpts {
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
    #[clap(long, default_value_t = 1)]
    format_version: u32,

    #[clap(long)]
    /// Maximum number of layers to use. The default value of 64 is chosen to
    /// balance splitting up an image into sufficient chunks versus
    /// compatibility with older OCI runtimes that may have problems
    /// with larger number of layers.
    ///
    /// However, with recent podman 5 for example with newer overlayfs,
    /// it works to use over 200 layers.
    max_layers: Option<NonZeroU32>,

    /// Tag to use for output image, or `latest` if unset.
    #[clap(long, default_value = "latest")]
    reference: String,

    /// Output image reference, in TRANSPORT:TARGET syntax.
    /// For example, `containers-storage:localhost/exampleos` or `oci:/path/to/ocidir`.
    #[clap(long, required = true)]
    output: String,
}

/// Generate a filesystem tree from an input manifest.
/// This can then be copied into e.g. a `FROM scratch` container image build.
#[derive(Debug, Parser)]
pub(crate) struct RootfsOpts {
    #[clap(long)]
    #[clap(value_parser)]
    /// Directory to use for caching downloaded packages and other data
    cachedir: Option<Utf8PathBuf>,

    /// Use this repository instead of a temporary one for debugging.
    #[clap(long, hide = true)]
    ostree_repo: Option<Utf8PathBuf>,

    /// Source root for package system configuration.
    #[clap(long, value_parser, conflicts_with = "source_root_rw")]
    source_root: Option<Utf8PathBuf>,

    #[clap(long, value_parser, conflicts_with = "source_root")]
    /// Rootfs to use for resolving package system configuration, such
    /// as the yum repository configuration, releasever, etc.
    ///
    /// The source root may be mutated to work around bugs.
    source_root_rw: Option<Utf8PathBuf>,

    /// Path to the input manifest
    manifest: Utf8PathBuf,

    /// Path to the target root filesystem tree.
    dest: Utf8PathBuf,
}

/// Unpack an OSTree commit to a target root in ostree-container layout (`bare-split-xattrs` format).
#[derive(Debug, Parser)]
pub(crate) struct CommitToContainerRootfsOpts {
    /// Path to OSTree repository
    #[clap(long, required = true)]
    repo: Utf8PathBuf,

    /// OSTree commit
    commit: String,

    /// Path to the target root filesystem tree.
    dest: Utf8PathBuf,
}

impl BuildChunkedOCIOpts {
    pub(crate) fn run(self) -> Result<()> {
        enum FileSource {
            Rootfs(Utf8PathBuf),
            Podman(Mount),
        }
        let rootfs_source = if let Some(rootfs) = self.rootfs {
            FileSource::Rootfs(rootfs)
        } else {
            let image = self.from.as_deref().unwrap();
            // TODO: Fix running this inside unprivileged podman too. We'll likely need
            // to refactor things into a two-step process where we do the mount+ostree repo commit
            // in a subprocess that has the "unshare", and then the secondary main process
            // just reads/operates on that.
            // Note that this would all be a lot saner with a composefs-native container storage
            // as we could cleanly operate on that, asking c/storage to synthesize one for us.
            // crate::containers_storage::reexec_if_needed()?;
            FileSource::Podman(Mount::new_for_image(image)?)
        };
        let rootfs = match &rootfs_source {
            FileSource::Rootfs(p) => p.as_path(),
            FileSource::Podman(mnt) => mnt.path(),
        };
        let rootfs = Dir::open_ambient_dir(rootfs, cap_std::ambient_authority())
            .with_context(|| format!("Opening {}", rootfs))?;
        // These must be set to exactly this; the CLI parser requires it.
        assert!(self.bootc);
        assert_eq!(self.format_version, 1);

        // If we're deriving from an existing image, be sure to preserve its metadata (labels, creation time, etc.)
        // by default.
        let image_config: oci_spec::image::ImageConfiguration =
            if let Some(image) = self.from.as_deref() {
                let img_transport = format!("containers-storage:{image}");
                Command::new("skopeo")
                    .args(["inspect", "--config", img_transport.as_str()])
                    .run_and_parse_json()
                    .context("Invoking skopeo to inspect config")?
            } else {
                // If we're not deriving, then we take the timestamp of the root
                // directory as a creation timestamp.
                let toplevel_ts = rootfs.dir_metadata()?.modified()?.into_std();
                let toplevel_ts = chrono::DateTime::<chrono::Utc>::from(toplevel_ts)
                    .to_rfc3339_opts(chrono::SecondsFormat::Secs, true);
                let mut config = ImageConfiguration::default();
                config.set_created(Some(toplevel_ts));
                config
            };
        let arch = image_config.architecture();
        let creation_timestamp = image_config
            .created()
            .as_deref()
            .map(chrono::DateTime::parse_from_rfc3339)
            .transpose()?;

        // Allocate a working temporary directory
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
        // Process the filesystem, generating an ostree commit
        let commitid =
            generate_commit_from_rootfs(&repo, &rootfs, modifier, creation_timestamp.as_ref())?;

        let label_arg = self
            .bootc
            .then_some(["--label", "containers.bootc=1"].as_slice())
            .unwrap_or_default();
        let base_config = image_config
            .config()
            .as_ref()
            .filter(|_| self.from.is_some());
        let config_data = if let Some(config) = base_config {
            let mut tmpf = tempfile::NamedTempFile::new()?;
            serde_json::to_writer(&mut tmpf, &config)?;
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
            .args(self.max_layers.map(|l| format!("--max-layers={l}")))
            .arg(format!("--arch={arch}"))
            .args(
                config_data
                    .iter()
                    .flat_map(|c| [OsStr::new("--image-config"), c.as_os_str()]),
            )
            .args([commitid.as_str(), self.output.as_str()])
            .run()
            .context("Invoking compose container-encapsulate")?;

        drop(rootfs);
        // Ensure our tempdir is only dropped now
        drop(td);
        match rootfs_source {
            FileSource::Rootfs(_) => {}
            FileSource::Podman(mnt) => {
                mnt.unmount().context("Final mount cleanup")?;
            }
        }

        Ok(())
    }
}

pub(crate) fn compose_build_chunked_oci_entrypoint(args: Vec<String>) -> CxxResult<()> {
    BuildChunkedOCIOpts::parse_from(args).run()?;
    Ok(())
}

/// Given a .repo file, rewrite all references to gpgkey=file:// inside
/// it to point into the source_root (if the key can be found there).
/// This is a workaround for https://github.com/coreos/rpm-ostree/issues/5285
fn mutate_one_dnf_repo(
    exec_root: &Dir,
    source_root: &Utf8Path,
    reposdir: &Dir,
    name: &Utf8Path,
) -> Result<()> {
    let r = reposdir.open(name).map(BufReader::new)?;
    let mut w = Vec::new();
    let mut modified = false;
    for line in r.lines() {
        let line = line?;
        // Extract the value of gpgkey=, if it doesn't match then
        // pass through the line.
        let Some(value) = line
            .split_once('=')
            .filter(|kv| kv.0 == "gpgkey")
            .map(|kv| kv.1)
        else {
            writeln!(w, "{line}")?;
            continue;
        };
        let mut gpg_modified = false;
        let mut updated_gpgkeys: Vec<Cow<str>> = Vec::new();
        for key in value.split_ascii_whitespace() {
            // If the gpg key isn't a local file, pass through the line.
            let Some(relpath) = key
                .strip_prefix("file://")
                .and_then(|path| path.strip_prefix('/'))
            else {
                updated_gpgkeys.push(key.into());
                continue;
            };
            // Handling variable substitutions here is painful, so we punt
            // if we find them and assume they should always be under the source root.
            let contains_varsubst = relpath.contains('$');
            // If it doesn't exist in the source root, then assume the absolute
            // reference is intentional.
            let target_repo_file = source_root.join(relpath);
            if !contains_varsubst && !exec_root.try_exists(&target_repo_file)? {
                tracing::debug!("Not present under source root: {target_repo_file}");
                updated_gpgkeys.push(key.into());
                continue;
            }
            gpg_modified = true;
            updated_gpgkeys.push(Cow::Owned(format!("file:///{target_repo_file}")));
        }
        if gpg_modified {
            modified = true;
            write!(w, "gpgkey=")?;
            for (i, key) in updated_gpgkeys.iter().enumerate() {
                if i != 0 {
                    write!(w, " ")?;
                }
                write!(w, "{key}")?;
            }
            writeln!(w)?;
        } else {
            writeln!(w, "{line}")?;
        }
    }
    if modified {
        tracing::debug!("Updated {name}");
        reposdir.write(name, w)?;
    } else {
        tracing::debug!("Unchanged repo file: {name}");
    }
    Ok(())
}

#[context("Preparing source root")]
fn mutate_source_root(exec_root: &Dir, source_root: &Utf8Path) -> Result<()> {
    let source_root_dir = exec_root
        .open_dir(source_root)
        .with_context(|| format!("Opening {source_root}"))?;
    if source_root_dir
        .symlink_metadata_optional(RPMOSTREE_RPMDB_LOCATION)?
        .is_none()
        && source_root_dir
            .symlink_metadata_optional(RPMOSTREE_SYSIMAGE_RPMDB)?
            .is_some()
    {
        source_root_dir
            .symlink_contents("../lib/sysimage/rpm", RPMOSTREE_RPMDB_LOCATION)
            .context("Symlinking rpmdb")?;
        println!("Symlinked {RPMOSTREE_RPMDB_LOCATION} in source root");
    }

    if !source_root_dir.try_exists("etc")? {
        return Ok(());
    }
    if let Some(repos) = source_root_dir
        .open_dir_optional("etc/yum.repos.d")
        .context("Opening yum.repos.d")?
    {
        for ent in repos.entries_utf8()? {
            let ent = ent?;
            if !ent.file_type()?.is_file() {
                continue;
            }
            let name: Utf8PathBuf = ent.file_name()?.into();
            let Some("repo") = name.extension() else {
                continue;
            };
            mutate_one_dnf_repo(exec_root, source_root, &repos, &name)?;
        }
    }

    Ok(())
}

fn fdpath_for(fd: impl AsFd, path: impl AsRef<Path>) -> PathBuf {
    let fd = fd.as_fd();
    let path = path.as_ref();
    let mut fdpath = PathBuf::from(format!("/proc/self/fd/{}", fd.as_raw_fd()));
    fdpath.push(path);
    fdpath
}

/// Get an optional extended attribute from the path; does not follow symlinks on the end target.
fn lgetxattr_optional_at(
    fd: impl AsFd,
    path: impl AsRef<Path>,
    key: impl AsRef<OsStr>,
) -> std::io::Result<Option<Vec<u8>>> {
    let fd = fd.as_fd();
    let path = path.as_ref();
    let key = key.as_ref();

    // Arbitrary hardcoded value, but we should have a better xattr API somewhere
    let mut value = [0u8; 8196];
    let fdpath = fdpath_for(fd, path);
    match rustix::fs::lgetxattr(&fdpath, key, &mut value) {
        Ok(r) => Ok(Some(Vec::from(&value[0..r]))),
        Err(e) if e == rustix::io::Errno::NODATA => Ok(None),
        Err(e) => Err(e.into()),
    }
}

#[derive(Debug, Default)]
struct XattrRemovalInfo {
    /// Set of unhandled xattrs we found
    names: BTreeSet<OsString>,
    /// Number of files with unhandled xattrsi
    count: u64,
}

fn strip_usermeta(d: &Dir, info: &mut XattrRemovalInfo) -> Result<()> {
    let usermeta_key = "user.ostreemeta";

    for ent in d.entries()? {
        let ent = ent?;
        let ty = ent.file_type()?;

        if ty.is_dir() {
            let subdir = ent.open_dir()?;
            strip_usermeta(&subdir, info)?;
        } else {
            let name = ent.file_name();
            let Some(usermeta) = lgetxattr_optional_at(d.as_fd(), &name, usermeta_key)? else {
                continue;
            };
            let usermeta =
                glib::Variant::from_data::<(u32, u32, u32, Vec<(Vec<u8>, Vec<u8>)>), _>(usermeta);
            let xattrs = usermeta.child_value(3);
            let n = xattrs.n_children();
            for i in 0..n {
                let v = xattrs.child_value(i);
                let key = v.child_value(0);
                let key = key.fixed_array::<u8>().unwrap();
                let key = OsStr::from_bytes(key);
                if !info.names.contains(key) {
                    info.names.insert(key.to_owned());
                }
                info.count += 1;
            }
            let fdpath = fdpath_for(d.as_fd(), &name);
            let _ = rustix::fs::lremovexattr(&fdpath, usermeta_key).context("lremovexattr")?;
        }
    }

    Ok(())
}

impl RootfsOpts {
    // For bad legacy reasons "compose install" actually writes to a subdirectory named rootfs.
    // Clean that up by deleting everything except rootfs/ and moving the contents of rootfs/
    // to the toplevel.
    //
    // Further, we change usr/etc back to etc
    fn fixup_installroot(d: &Dir) -> Result<()> {
        let rootfs_name = "rootfs";
        for ent in d.entries()? {
            let ent = ent?;
            let name = ent.file_name();
            if let Some("rootfs") = name.to_str() {
                continue;
            }
            d.remove_all_optional(&name)?;
        }
        let rootfs = d.open_dir(rootfs_name)?;
        for ent in rootfs.entries().context("Reading rootfs")? {
            let ent = ent?;
            let name = ent.file_name();
            rootfs
                .rename(&name, d, &name)
                .with_context(|| format!("Renaming rootfs/{name:?}"))?;
        }
        // Clean up the now empty dir
        d.remove_dir(rootfs_name).context("Removing rootfs")?;

        // Propagate mode bits from source to target
        {
            let perms = rootfs.dir_metadata()?.permissions();
            d.set_permissions(".", perms)
                .context("Setting target permissions")?;
            tracing::debug!("rootfs fixup complete");
        }

        // And finally, clean up the ostree.usermeta xattr
        let mut info = XattrRemovalInfo::default();
        strip_usermeta(d, &mut info)?;
        if info.count > 0 {
            eprintln!("Found unhandled xattrs in files: {}", info.count);
            for attr in info.names {
                eprintln!("  {attr:?}");
            }
        }

        Ok(())
    }

    pub(crate) fn run(mut self) -> Result<()> {
        let manifest = self.manifest.as_path();

        if self.dest.try_exists()? {
            anyhow::bail!("Refusing to operate on extant target {}", self.dest);
        }

        // If we were passed a mutable source root, then let's work around some bugs
        if let Some(rw) = self.source_root_rw.take() {
            // Clap ensures this
            assert!(self.source_root.is_none());
            let exec_root = Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
            // We could handle relative paths, but it's easier to require absolute.
            // The mutation work below all happens via cap-std because it's way easier
            // to unit test with that.
            let Some(rw_relpath) = rw.strip_prefix("/").ok() else {
                anyhow::bail!("Expected absolute path for source-root: {rw}");
            };
            mutate_source_root(&exec_root, rw_relpath)?;
            self.source_root = Some(rw);
        }

        // Create a temporary directory for things
        let td = tempfile::tempdir_in("/var/tmp")?;
        let td_path: Utf8PathBuf = td.path().to_owned().try_into()?;

        // If we're passed an ostree repo, open it.
        let repo_path = if let Some(ostree_repo) = self.ostree_repo {
            ostree_repo
        } else {
            // Otherwise make a temporary ostree repo
            let repo_path = td_path.join("repo");
            let repo = ostree::Repo::create_at(
                libc::AT_FDCWD,
                repo_path.as_str(),
                ostree::RepoMode::Bare,
                None,
                gio::Cancellable::NONE,
            )?;
            drop(repo);
            repo_path
        };

        // Just build the root filesystem tree
        self_command()
            .args([
                "compose",
                "install",
                // We can't rely on being able to do labels in a container build
                // and instead assume that bootc will do client side labeling.
                "--disable-selinux",
                "--unified-core",
                "--postprocess",
                "--repo",
                repo_path.as_str(),
            ])
            .args(
                self.cachedir
                    .iter()
                    .flat_map(|v| ["--cachedir", v.as_str()]),
            )
            .args(
                self.source_root
                    .iter()
                    .flat_map(|v| ["--source-root", v.as_str()]),
            )
            .args([manifest.as_str(), self.dest.as_str()])
            .run()
            .context("Executing compose install")?;

        // Clear everything in the tempdir; at this point we may have hardlinks into
        // the pkgcache repo, which we don't need because we're producing a flat
        // tree, not a repo.
        td.close()?;

        // Undo the subdirectory "rootfs"
        {
            let target = Dir::open_ambient_dir(&self.dest, cap_std::ambient_authority())?;
            Self::fixup_installroot(&target)?;
        }

        // After compose install/postprocess we still have usr/etc, not etc.
        // Since we're generating a plain root and not an ostree commit, let's
        // move it.
        let target = Dir::open_ambient_dir(&self.dest, cap_std::ambient_authority())?;
        let etc = "etc";
        let usretc = "usr/etc";
        if target.try_exists(usretc)? {
            tracing::debug!("Renaming {usretc} to {etc}");
            target
                .rename(usretc, &target, etc)
                .context("Renaming usr/etc to etc")?;
        }
        Ok(())
    }
}

pub(crate) fn compose_rootfs_entrypoint(args: Vec<String>) -> CxxResult<()> {
    RootfsOpts::parse_from(args).run()?;
    Ok(())
}

impl CommitToContainerRootfsOpts {
    /// Execute `compose commit-to-container-rootfs`. This just:
    /// - Opens up the target ostree repo
    /// - Creates a copy of it to the target root (note: no hardlinks from repo).
    pub(crate) fn run(self) -> Result<()> {
        let cancellable = gio::Cancellable::NONE;
        let repo = ostree::Repo::open_at(libc::AT_FDCWD, self.repo.as_str(), cancellable)?;
        unpack_commit_to_dir_as_bare_split_xattrs(&repo, &self.commit, &self.dest)
    }
}

/// For the ostree-container format, we added a new repo mode `bare-split-xattrs`.
/// While the ostree (C) code base has some support for reading this, it does
/// not support writing it. The only code that does "writes" is when we generate
/// a tar stream in the ostree-ext codebase. Hence, we synthesize the flattened
/// rootfs here by converting to a tar stream internally, and unpacking it via
/// forking `tar -x`.
fn unpack_commit_to_dir_as_bare_split_xattrs(
    repo: &ostree::Repo,
    rev: &str,
    path: &Utf8Path,
) -> Result<()> {
    std::fs::create_dir(path)?;
    let repo = repo.clone();

    // I hit some bugs in the Rust tar-rs trying to use it for this,
    // would probably be good to fix, but in the end there's no
    // issues with relying on /bin/tar here.
    let mut untar_cmd = Command::new("tar");
    untar_cmd.stdin(std::process::Stdio::piped());
    // We default to all xattrs *except* selinux (because we can't set it
    // at container build time).
    untar_cmd.current_dir(path).args([
        "-x",
        "--xattrs",
        "--xattrs-include=*",
        "--no-selinux",
        "-f",
        "-",
    ]);
    let mut untar_child = untar_cmd.spawn()?;
    // To ensure any reference to the inner pipes are closed
    drop(untar_cmd);
    let stdin = untar_child.stdin.take().unwrap();
    // We use a thread scope so our spawned helper thread to synthesize
    // the tar can safely borrow from this outer scope. Which doesn't
    // *really* matter since we're just borrowing repo and rev, but hey might
    // as well avoid copies.
    std::thread::scope(move |scope| {
        tracing::debug!("spawning untar");
        let mktar = scope.spawn(move || {
            tracing::debug!("spawning mktar");
            ostree_ext::tar::export_commit(&repo, &rev, stdin, None)?;
            anyhow::Ok(())
        });
        // Wait for both of our tasks.
        tracing::debug!("joining mktar");
        let mktar_result = mktar.join().unwrap();
        tracing::debug!("completed mktar");
        let untar_result = untar_child.wait()?;
        tracing::debug!("completed untar");
        let untar_result = if !untar_result.success() {
            Err(anyhow::anyhow!("failed to untar: {untar_result:?}"))
        } else {
            Ok(())
        };
        // Handle errors from either end, or both. Almost always it will be
        // "both" - if one side fails, the other will get EPIPE usually.
        match (mktar_result, untar_result) {
            (Ok(()), Ok(())) => anyhow::Ok(()),
            (Ok(()), Err(e)) => return Err(e.into()),
            (Err(e), Ok(())) => return Err(e.into()),
            (Err(mktar_err), Err(untar_err)) => {
                anyhow::bail!(
                    "Multiple errors:\n Generating tar: {mktar_err}\n Unpacking: {untar_err}"
                );
            }
        }
    })
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

#[context("Generating commit from rootfs")]
fn generate_commit_from_rootfs(
    repo: &ostree::Repo,
    rootfs: &Dir,
    modifier: ostree::RepoCommitModifier,
    creation_time: Option<&chrono::DateTime<chrono::FixedOffset>>,
) -> Result<String> {
    let root_mtree = ostree::MutableTree::new();
    let cancellable = gio::Cancellable::NONE;
    let tx = repo.auto_transaction(cancellable)?;

    let policy = ostree::SePolicy::new_at(rootfs.as_fd().as_raw_fd(), cancellable)?;
    modifier.set_sepolicy(Some(&policy));

    let root_dirmeta = create_root_dirmeta(rootfs, &policy)?;
    let root_metachecksum = repo
        .write_metadata(
            ostree::ObjectType::DirMeta,
            None,
            &root_dirmeta,
            cancellable,
        )
        .context("Writing root dirmeta")?;
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
            )
            .with_context(|| format!("Processing dir {name}"))?;
        } else if ftype.is_symlink() {
            let contents: Utf8PathBuf = rootfs
                .read_link_contents(&name)
                .with_context(|| format!("Reading {name}"))?
                .try_into()?;
            // Label lookups need to be absolute
            let selabel_path = format!("/{name}");
            let label = policy.label(selabel_path.as_str(), 0o777 | libc::S_IFLNK, cancellable)?;
            let xattrs = label_to_xattrs(label.as_deref());
            let link_checksum = repo
                .write_symlink(None, 0, 0, xattrs.as_ref(), contents.as_str(), cancellable)
                .with_context(|| format!("Processing symlink {selabel_path}"))?;
            root_mtree.replace_file(&name, &link_checksum)?;
        } else {
            // Yes we could support this but it's a surprising amount of typing
            anyhow::bail!("Unsupported regular file {name} at toplevel");
        }
    }

    postprocess_mtree(repo, &root_mtree)?;

    let ostree_root = repo.write_mtree(&root_mtree, cancellable)?;
    let ostree_root = ostree_root.downcast_ref::<ostree::RepoFile>().unwrap();
    let creation_time: u64 = creation_time
        .as_ref()
        .map(|t| t.timestamp())
        .unwrap_or_default()
        .try_into()
        .context("Parsing creation time")?;
    let commit = repo.write_commit_with_time(
        None,
        None,
        None,
        None,
        ostree_root,
        creation_time,
        cancellable,
    )?;

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
    let cancellable = gio::Cancellable::NONE;

    let opt = ComposeImageOpts::parse_from(args.iter().skip(1));

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
    use rustix::{fs::XattrFlags, io::Errno};

    use super::*;

    #[test]
    fn test_fixup_install_root() -> Result<()> {
        let td = cap_tempfile::tempdir(cap_std::ambient_authority())?;
        let bashpath = Utf8Path::new("rootfs/usr/bin/bash");

        td.create_dir_all(bashpath.parent().unwrap())?;
        td.write(bashpath, b"bash")?;
        let f = td.open(bashpath)?;
        let xattrs = Vec::<(Vec<u8>, Vec<u8>)>::new();
        let v = glib::Variant::from((0u32, 0u32, 0u32, xattrs));
        let v = v.data_as_bytes();
        rustix::fs::fsetxattr(f.as_fd(), "user.ostreemeta", &v, XattrFlags::empty())
            .context("fsetxattr")?;

        RootfsOpts::fixup_installroot(&td).unwrap();

        let f = td.open("usr/bin/bash").unwrap();
        let mut buf = [0u8; 1024];
        let e = rustix::fs::fgetxattr(f.as_fd(), "user.ostreemeta", &mut buf).err();
        assert_eq!(e, Some(Errno::NODATA));

        Ok(())
    }

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

        let commit = generate_commit_from_rootfs(&repo, &td, modifier.clone(), None).unwrap();
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

        let ts = chrono::DateTime::parse_from_rfc2822("Fri, 29 Aug 1997 10:30:42 PST").unwrap();
        let commit = generate_commit_from_rootfs(&repo, &td, modifier.clone(), Some(&ts)).unwrap();
        assert_eq!(
            commit,
            "1423c43d7b76207dc86b357a4834fcea444fcb2ee3a81541fbfbd52a85e05bc3"
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

    #[test]
    fn test_unpack() -> Result<()> {
        // Skip without the ostree binary since ostree-devel doesn't pull it in
        if !Utf8Path::new("/usr/bin/ostree").try_exists()? {
            return Ok(());
        }
        let td = tempfile::tempdir()?;
        let td_path: Utf8PathBuf = td.path().to_owned().try_into()?;
        let repo_path = td_path.join("repo");
        let repo = ostree::Repo::create_at(
            libc::AT_FDCWD,
            repo_path.as_str(),
            ostree::RepoMode::BareUser,
            None,
            gio::Cancellable::NONE,
        )?;
        let rootfs = td_path.join("rootfs");
        std::fs::create_dir(&rootfs)?;
        let testpath = rootfs.join("test.txt");
        std::fs::write(&testpath, b"Test")?;
        rustix::fs::setxattr(
            testpath.as_std_path(),
            "user.test".as_bytes(),
            b"somevalue",
            rustix::fs::XattrFlags::empty(),
        )
        .context("setxattr")?;
        Command::new("ostree")
            .args(["--repo=repo", "commit", "-b", "test", "--tree=dir=rootfs"])
            .current_dir(&td_path)
            .run()?;
        let rev = repo.require_rev("test")?;
        let unpack_path = td_path.join("rootfs2");
        unpack_commit_to_dir_as_bare_split_xattrs(&repo, &rev, &unpack_path)?;
        let testpath = unpack_path.join("test.txt");
        assert!(testpath.try_exists()?);
        let mut buf = [0u8; 1024];
        let n = rustix::fs::getxattr(testpath.as_std_path(), "user.test".as_bytes(), &mut buf)
            .context("getxattr")?;
        let buf = std::str::from_utf8(&buf[0..n]).unwrap();
        assert_eq!(buf, "somevalue");

        Ok(())
    }

    #[test]
    fn test_mutate_source_root() -> Result<()> {
        let rootfs = &cap_tempfile::TempDir::new(cap_std::ambient_authority())?;
        // Should be a no-op in an empty root
        rootfs.create_dir("repos")?;
        mutate_source_root(rootfs, "repos".into()).unwrap();
        rootfs.create_dir_all("repos/usr/share")?;
        rootfs.create_dir_all("repos/usr/lib/sysimage/rpm")?;
        mutate_source_root(rootfs, "repos".into()).unwrap();
        assert!(rootfs
            .symlink_metadata("repos/usr/share/rpm")
            .unwrap()
            .is_symlink());

        rootfs.create_dir_all("repos/etc/yum.repos.d")?;
        rootfs.create_dir_all("repos/etc/pki/rpm-gpg")?;
        rootfs.write("repos/etc/pki/rpm-gpg/repo2.key", "repo2 gpg key")?;
        let orig_repo_content = indoc::indoc! { r#"
        [repo]
        baseurl=blah
        gpgkey=https://example.com

        [repo2]
        baseurl=other
        gpgkey=file:///etc/pki/rpm-gpg/repo2.key

        [repo4]
        baseurl=other
        # These keys don't exist in the source root, but we rewrite anyways
        gpgkey=file:///etc/pki/rpm-gpg/some-key-with-$releasever-and-$basearch file:///etc/pki/rpm-gpg/another-key-with-$releasever-and-$basearch

        [repo3]
        baseurl=blah
        gpgkey=file:///absolute/path/not-in-source-root
    "#};
        rootfs.write("repos/etc/yum.repos.d/test.repo", orig_repo_content)?;
        mutate_source_root(rootfs, "repos".into()).unwrap();
        let found_repos = rootfs.read_to_string("repos/etc/yum.repos.d/test.repo")?;
        let expected = indoc::indoc! { r#"
        [repo]
        baseurl=blah
        gpgkey=https://example.com

        [repo2]
        baseurl=other
        gpgkey=file:///repos/etc/pki/rpm-gpg/repo2.key

        [repo4]
        baseurl=other
        # These keys don't exist in the source root, but we rewrite anyways
        gpgkey=file:///repos/etc/pki/rpm-gpg/some-key-with-$releasever-and-$basearch file:///repos/etc/pki/rpm-gpg/another-key-with-$releasever-and-$basearch

        [repo3]
        baseurl=blah
        gpgkey=file:///absolute/path/not-in-source-root
    "#};
        similar_asserts::assert_eq!(expected, found_repos);

        Ok(())
    }
}
