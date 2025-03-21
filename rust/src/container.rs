//! CLI exposing `ostree-rs-ext container`

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fmt::Debug;
use std::fs::File;
use std::io::BufReader;
use std::num::NonZeroU32;
use std::process::Command;
use std::rc::Rc;

use anyhow::{Context, Result};
use camino::{Utf8Path, Utf8PathBuf};
use cap_std::fs::Dir;
use cap_std_ext::cap_std;
use cap_std_ext::prelude::*;
use chrono::prelude::*;
use clap::Parser;
use fn_error_context::context;
use ostree::glib;
use ostree_ext::chunking::ObjectMetaSized;
use ostree_ext::container::{Config, ExportOpts, ImageReference};
use ostree_ext::containers_image_proxy;
use ostree_ext::objectsource::{
    ContentID, ObjectMeta, ObjectMetaMap, ObjectMetaSet, ObjectSourceMeta,
};
use ostree_ext::oci_spec::image::{Arch, Os, PlatformBuilder};
use ostree_ext::prelude::*;
use ostree_ext::{gio, oci_spec, ostree};

use crate::cmdutils::CommandRunExt;
use crate::cxxrsutil::FFIGObjectReWrap;
use crate::fsutil::{self, FileHelpers, ResolvedOstreePaths};
use crate::progress::progress_task;
use crate::CxxResult;

#[derive(Debug, Parser)]
struct ContainerEncapsulateOpts {
    #[clap(long)]
    #[clap(value_parser)]
    repo: Utf8PathBuf,

    /// OSTree branch name or checksum
    ostree_ref: String,

    /// Image reference, e.g. registry:quay.io/exampleos/exampleos:latest
    #[clap(value_parser = ostree_ext::cli::parse_base_imgref)]
    imgref: ImageReference,

    /// Additional labels for the container
    #[clap(name = "label", long, short)]
    labels: Vec<String>,

    /// Path to container image configuration in JSON format.  This is the `config`
    /// field of https://github.com/opencontainers/image-spec/blob/main/config.md
    #[clap(long)]
    image_config: Option<Utf8PathBuf>,

    /// Override the architecture.
    #[clap(long)]
    arch: Option<Arch>,

    /// Propagate an OSTree commit metadata key to container label
    #[clap(name = "copymeta", long)]
    copy_meta_keys: Vec<String>,

    /// Propagate an optionally-present OSTree commit metadata key to container label
    #[clap(name = "copymeta-opt", long)]
    copy_meta_opt_keys: Vec<String>,

    /// Corresponds to the Dockerfile `CMD` instruction.
    #[clap(long)]
    cmd: Option<Vec<String>>,

    /// Maximum number of container image layers
    #[clap(long)]
    max_layers: Option<NonZeroU32>,

    /// The encapsulated container format version; must be 0 or 1.
    #[clap(long, default_value = "1")]
    format_version: u32,

    #[clap(long)]
    /// Output content metadata as JSON
    write_contentmeta_json: Option<Utf8PathBuf>,

    /// Compare OCI layers of current build with another(imgref)
    #[clap(name = "compare-with-build", long)]
    compare_with_build: Option<String>,

    /// Prevent a change in packing structure by taking a previous build metadata (oci config and
    /// manifest)
    #[clap(long)]
    previous_build_manifest: Option<Utf8PathBuf>,
}

#[derive(Debug)]
struct MappingBuilder {
    /// Maps from package ID to metadata
    packagemeta: ObjectMetaSet,

    /// Maps from object checksum to absolute filesystem path
    checksum_paths: BTreeMap<String, BTreeSet<Utf8PathBuf>>,

    /// Maps from absolute filesystem path to the package IDs that
    /// provide it
    path_packages: HashMap<Utf8PathBuf, BTreeSet<ContentID>>,

    unpackaged_id: ContentID,

    /// Files that were processed before the global tree walk
    skip: HashSet<Utf8PathBuf>,

    /// Size according to RPM database
    rpmsize: u64,
}

impl MappingBuilder {
    /// For now, we stick everything that isn't a package inside a single "unpackaged" state.
    /// In the future though if we support e.g. containers in /usr/share/containers or the
    /// like, this will need to change.
    const UNPACKAGED_ID: &'static str = "rpmostree-unpackaged-content";

    fn duplicate_objects(&self) -> impl Iterator<Item = (&String, &BTreeSet<Utf8PathBuf>)> {
        self.checksum_paths
            .iter()
            .filter(|(_, paths)| paths.len() > 1)
    }

    fn multiple_owners(&self) -> impl Iterator<Item = (&Utf8PathBuf, &BTreeSet<ContentID>)> {
        self.path_packages.iter().filter(|(_, pkgs)| pkgs.len() > 1)
    }
}

impl From<MappingBuilder> for ObjectMeta {
    fn from(b: MappingBuilder) -> ObjectMeta {
        let mut content = ObjectMetaMap::default();
        for (checksum, paths) in b.checksum_paths {
            // Use the first package name found for one of the paths (if multiple). These
            // are held in sorted data structures, so this should be deterministic.
            //
            // If not found, use the unpackaged name.
            let pkg = paths
                .iter()
                .filter_map(|p| b.path_packages.get(p).map(|pkgs| pkgs.first().unwrap()))
                .next()
                .unwrap_or(&b.unpackaged_id);

            content.insert(checksum, pkg.clone());
        }

        ObjectMeta {
            map: content,
            set: b.packagemeta,
        }
    }
}

/// Walk over the whole filesystem, and generate mappings from content object checksums
/// to the path that provides them.
fn build_fs_mapping_recurse(
    path: &mut Utf8PathBuf,
    dir: &gio::File,
    state: &mut MappingBuilder,
) -> Result<()> {
    let e = dir.enumerate_children(
        "standard::name,standard::type",
        gio::FileQueryInfoFlags::NOFOLLOW_SYMLINKS,
        gio::Cancellable::NONE,
    )?;
    for child in e {
        let childi = child?;
        let name: Utf8PathBuf = childi.name().try_into()?;
        let child = dir.child(&name);
        path.push(&name);
        match childi.file_type() {
            gio::FileType::Regular | gio::FileType::SymbolicLink => {
                let child = child.downcast::<ostree::RepoFile>().unwrap();

                // Remove the skipped path, since we can't hit it again.
                if state.skip.remove(Utf8Path::new(path)) {
                    path.pop();
                    continue;
                }

                // Ensure there's a checksum -> path entry. If it was previously
                // accounted for by a package, this is essentially a no-op. If not,
                // there'll be no corresponding path -> package entry, and the packaging
                // operation will treat the file as being "unpackaged".
                let checksum = child.checksum().to_string();
                state
                    .checksum_paths
                    .entry(checksum)
                    .or_default()
                    .insert(path.clone());
            }
            gio::FileType::Directory => {
                build_fs_mapping_recurse(path, &child, state)?;
            }
            o => anyhow::bail!("Unhandled file type: {o:?}"),
        }
        path.pop();
    }
    Ok(())
}

fn gv_nevra_to_string(pkg: &glib::Variant) -> String {
    let name = pkg.child_value(0);
    let name = name.str().unwrap();
    let epoch = pkg.child_value(1);
    let epoch = epoch.str().unwrap();
    let version = pkg.child_value(2);
    let version = version.str().unwrap();
    let release = pkg.child_value(3);
    let release = release.str().unwrap();
    let arch = pkg.child_value(4);
    let arch = arch.str().unwrap();
    if epoch == "0" {
        format!("{}-{}-{}.{}", name, version, release, arch)
    } else {
        format!("{}-{}:{}-{}.{}", name, epoch, version, release, arch)
    }
}

async fn compare_builds(old_build: &str, new_build: &str) -> Result<()> {
    let proxy = containers_image_proxy::ImageProxy::new().await?;
    let oi_old = proxy.open_image(old_build).await?;
    let (_, manifest_old) = proxy.fetch_manifest(&oi_old).await?;
    let oi_now = proxy.open_image(new_build).await?;
    let (_, new_manifest) = proxy.fetch_manifest(&oi_now).await?;
    let diff = ostree_ext::container::ManifestDiff::new(&manifest_old, &new_manifest);
    diff.print();
    Ok(())
}

/// Like `ostree container encapsulate`, but uses chunks derived from package data.
pub fn container_encapsulate(args: Vec<String>) -> CxxResult<()> {
    let args = args.iter().skip(1).map(|s| s.as_str());
    let opt = ContainerEncapsulateOpts::parse_from(args);
    let repo = &ostree_ext::cli::parse_repo(&opt.repo)?;
    let (root, rev) = repo.read_commit(opt.ostree_ref.as_str(), gio::Cancellable::NONE)?;
    let cancellable = gio::Cancellable::new();
    let pkglist: glib::Variant = {
        let r = crate::ffi::package_variant_list_for_commit(
            repo.reborrow_cxx(),
            rev.as_str(),
            cancellable.reborrow_cxx(),
        )
        .context("Reading package variant list")?;
        unsafe { glib::translate::from_glib_full(r as *mut _) }
    };

    // Open the RPM database for this commit.
    let q =
        crate::ffi::rpmts_for_commit(repo.reborrow_cxx(), rev.as_str()).context("Getting refts")?;

    let mut state = MappingBuilder {
        unpackaged_id: Rc::from(MappingBuilder::UNPACKAGED_ID),
        packagemeta: Default::default(),
        checksum_paths: Default::default(),
        path_packages: Default::default(),
        skip: Default::default(),
        rpmsize: Default::default(),
    };
    // Insert metadata for unpackaged content.
    state.packagemeta.insert(ObjectSourceMeta {
        identifier: Rc::clone(&state.unpackaged_id),
        name: Rc::clone(&state.unpackaged_id),
        srcid: Rc::clone(&state.unpackaged_id),
        // Assume that content in here changes frequently.
        change_time_offset: u32::MAX,
        change_frequency: u32::MAX,
    });

    let mut lowest_change_time = None;
    let mut highest_change_time = None;
    let mut package_meta = HashMap::new();
    if pkglist.n_children() == 0 {
        return Err("Failed to find any packages".to_owned().into());
    }
    for pkg in pkglist.iter() {
        let name = pkg.child_value(0);
        let name = name.str().unwrap();
        let arch = pkg.child_value(4);
        let arch = arch.str().unwrap();
        let nevra = Rc::from(gv_nevra_to_string(&pkg).into_boxed_str());
        let pkgmeta = q
            .package_meta(name, arch)
            .context("Querying package meta")?;
        let buildtime = pkgmeta.buildtime();
        if let Some((lowid, lowtime)) = lowest_change_time.as_mut() {
            if *lowtime > buildtime {
                *lowid = Rc::clone(&nevra);
                *lowtime = buildtime;
            }
        } else {
            lowest_change_time = Some((Rc::clone(&nevra), pkgmeta.buildtime()))
        }
        if let Some(hightime) = highest_change_time.as_mut() {
            if *hightime < buildtime {
                *hightime = buildtime;
            }
        } else {
            highest_change_time = Some(pkgmeta.buildtime())
        }
        state.rpmsize += pkgmeta.size();
        package_meta.insert(nevra, pkgmeta);
    }

    // SAFETY: There must be at least one package; checked above.
    let (lowest_change_name, lowest_change_time) =
        lowest_change_time.expect("Failed to find any packages");
    let highest_change_time = highest_change_time.expect("Failed to find any packages");

    // Walk over the packages, and generate the `packagemeta` mapping, which is basically a subset of
    // package metadata abstracted for ostree.  Note that right now, the package metadata includes
    // both a "unique identifer" and a "human readable name", but for rpm-ostree we're just making
    // those the same thing.
    for (nevra, pkgmeta) in package_meta.iter() {
        let buildtime = pkgmeta.buildtime();
        let change_time_offset_secs: u32 = buildtime
            .checked_sub(lowest_change_time)
            .unwrap()
            .try_into()
            .unwrap();
        // Convert to hours, because there's no strong use for caring about the relative difference of builds in terms
        // of minutes or seconds.
        let change_time_offset = change_time_offset_secs / (60 * 60);
        let changelogs = pkgmeta.changelogs();
        // Ignore the updates to packages more than a year away from the latest built package as its
        // contribution becomes increasingly irrelevant to the likelihood of the package updating
        // in the future
        // TODO: Weighted Moving Averages (Weights decaying by year) to calculate the frequency
        let pruned_changelogs: Vec<&u64> = changelogs
            .iter()
            .filter(|e| {
                let curr_build = glib::DateTime::from_unix_utc(**e as i64).unwrap();
                let highest_time_build =
                    glib::DateTime::from_unix_utc(highest_change_time as i64).unwrap();
                highest_time_build.difference(&curr_build).as_days() <= 365_i64
            })
            .collect();
        state.packagemeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(nevra),
            name: Rc::from(libdnf_sys::hy_split_nevra(nevra)?.name),
            srcid: Rc::from(pkgmeta.src_pkg()),
            change_time_offset,
            change_frequency: pruned_changelogs.len() as u32,
        });
    }

    let kernel_dir = ostree_ext::bootabletree::find_kernel_dir(&root, gio::Cancellable::NONE)?;
    if let Some(kernel_dir) = kernel_dir {
        let kernel_ver: Utf8PathBuf = kernel_dir
            .basename()
            .unwrap()
            .try_into()
            .map_err(anyhow::Error::msg)?;
        let initramfs = kernel_dir.child("initramfs.img");
        if initramfs.query_exists(gio::Cancellable::NONE) {
            let path: Utf8PathBuf = initramfs
                .path()
                .unwrap()
                .try_into()
                .map_err(anyhow::Error::msg)?;
            let initramfs = initramfs.downcast_ref::<ostree::RepoFile>().unwrap();
            let checksum = initramfs.checksum();
            let name = "initramfs".to_string();
            let identifier = format!("{} (kernel {})", name, kernel_ver).into_boxed_str();
            let identifier = Rc::from(identifier);

            state
                .checksum_paths
                .entry(checksum.to_string())
                .or_default()
                .insert(path.clone());
            state
                .path_packages
                .entry(path.clone())
                .or_default()
                .insert(Rc::clone(&identifier));
            state.packagemeta.insert(ObjectSourceMeta {
                identifier: Rc::clone(&identifier),
                name: Rc::from(name),
                srcid: Rc::clone(&identifier),
                change_time_offset: u32::MAX,
                change_frequency: u32::MAX,
            });
            state.skip.insert(path);
        }
    }

    let rpmdb = root.resolve_relative_path(crate::composepost::RPMOSTREE_RPMDB_LOCATION);
    if rpmdb.query_exists(gio::Cancellable::NONE) {
        // TODO add mapping for rpmdb
    }

    progress_task("Building package mapping", || {
        // Walk each package, adding mappings for each of the files it provides
        let mut dir_cache: HashMap<Utf8PathBuf, ResolvedOstreePaths> = HashMap::new();
        for (nevra, pkgmeta) in package_meta.iter() {
            for path in pkgmeta.provided_paths()? {
                let path = Utf8PathBuf::from(path);

                // Resolve the path to its ostree file
                if let Some(ostree_paths) = fsutil::resolve_ostree_paths(
                    &path,
                    root.downcast_ref::<ostree::RepoFile>().unwrap(),
                    &mut dir_cache,
                ) {
                    if ostree_paths.path.is_regular() || ostree_paths.path.is_symlink() {
                        let real_path =
                            Utf8PathBuf::from_path_buf(ostree_paths.path.peek_path().unwrap())
                                .unwrap();
                        let checksum = ostree_paths.path.checksum().to_string();

                        state
                            .checksum_paths
                            .entry(checksum)
                            .or_default()
                            .insert(real_path.clone());
                        state
                            .path_packages
                            .entry(real_path)
                            .or_default()
                            .insert(Rc::clone(nevra));
                    }
                }
            }
        }

        // Then, walk the file system marking any remainders as unpackaged
        build_fs_mapping_recurse(&mut Utf8PathBuf::from("/"), &root, &mut state)
    })?;

    let src_pkgs: HashSet<_> = state.packagemeta.iter().map(|p| &p.srcid).collect();

    // Print out information about what we found
    println!(
        "{} objects in {} packages ({} source)",
        state.checksum_paths.len(),
        state.packagemeta.len(),
        src_pkgs.len(),
    );
    println!("rpm size: {}", state.rpmsize);
    println!(
        "Earliest changed package: {} at {}",
        lowest_change_name,
        Utc.timestamp_opt(lowest_change_time.try_into().unwrap(), 0)
            .unwrap()
    );
    println!("Duplicates: {}", state.duplicate_objects().count());
    println!("Multiple owners: {}", state.multiple_owners().count());

    // Convert our build state into the state that ostree consumes, discarding
    // transient data such as the cases of files owned by multiple packages.
    let meta: ObjectMeta = state.into();

    // Now generate the sized version
    let meta = ObjectMetaSized::compute_sizes(repo, meta)?;
    if let Some(v) = opt.write_contentmeta_json {
        let v = v.strip_prefix("/").unwrap_or(&v);
        let root = Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
        root.atomic_replace_with(v, |w| {
            serde_json::to_writer(w, &meta.sizes).map_err(anyhow::Error::msg)
        })?;
    }
    // TODO: Put this in a public API in ostree-rs-ext?
    let labels = opt
        .labels
        .into_iter()
        .map(|l| {
            let (k, v) = l
                .split_once('=')
                .ok_or_else(|| anyhow::anyhow!("Missing '=' in label {}", l))?;
            Ok((k.to_string(), v.to_string()))
        })
        .collect::<Result<_>>()?;

    let package_structure = opt
        .previous_build_manifest
        .as_ref()
        .map(|p| {
            oci_spec::image::ImageManifest::from_file(p)
                .map_err(|e| anyhow::anyhow!("Failed to read previous manifest {p}: {e}"))
        })
        .transpose()?;

    // Default to copying the input hash to support cheap change detection
    let copy_meta_opt_keys = opt
        .copy_meta_opt_keys
        .into_iter()
        .chain(std::iter::once("rpmostree.inputhash".to_owned()))
        .collect();

    let config = Config {
        labels: Some(labels),
        cmd: opt.cmd,
    };
    let mut opts = ExportOpts::default();
    opts.copy_meta_keys = opt.copy_meta_keys;
    opts.copy_meta_opt_keys = copy_meta_opt_keys;
    opts.max_layers = opt.max_layers;
    opts.prior_build = package_structure.as_ref();
    opts.contentmeta = Some(&meta);
    if let Some(config_path) = opt.image_config.as_deref() {
        let config = serde_json::from_reader(File::open(config_path).map(BufReader::new)?)
            .map_err(anyhow::Error::msg)?;
        opts.container_config = Some(config);
    }
    // If an architecture was provided, then generate a new Platform (using the host OS type)
    // but override with that architecture.
    if let Some(arch) = opt.arch.as_ref() {
        let platform = PlatformBuilder::default()
            .architecture(arch.clone())
            .os(Os::default())
            .build()
            .unwrap();
        opts.platform = Some(platform);
    }
    let handle = tokio::runtime::Handle::current();
    println!("Generating container image");
    let digest = handle.block_on(async {
        ostree_ext::container::encapsulate(repo, rev.as_str(), &config, Some(opts), &opt.imgref)
            .await
            .context("Encapsulating")
    })?;

    if let Some(compare_with_build) = opt.compare_with_build.as_ref() {
        progress_task("Comparing Builds", || {
            handle.block_on(async {
                compare_builds(compare_with_build, &format!("{}", &opt.imgref)).await
            })
        })?;
    };

    println!("Pushed digest: {}", digest);
    Ok(())
}

#[derive(clap::Parser)]
struct UpdateFromRunningOpts {
    /// Path to target system root
    #[clap(value_parser)]
    target_root: Utf8PathBuf,

    #[clap(long)]
    /// Reboot after performing operation
    reboot: bool,
}

// This reimplements https://github.com/ostreedev/ostree/pull/2691 basically
#[context("Finding encapsulated commits")]
fn find_encapsulated_commits(repo: &Utf8Path) -> Result<Vec<String>> {
    let objects = Dir::open_ambient_dir(repo.join("objects"), cap_std::ambient_authority())?;
    let mut r = Vec::new();
    for entry in objects.entries()? {
        let entry = entry?;
        let etype = entry.file_type()?;
        if !etype.is_dir() {
            continue;
        }
        let name = entry.file_name();
        let name = if let Some(n) = name.to_str() {
            n
        } else {
            continue;
        };

        let subd = entry.open_dir()?;
        for entry in subd.entries()? {
            let entry = entry?;
            let etype = entry.file_type()?;
            if !etype.is_file() {
                continue;
            }
            let subname = entry.file_name();
            let subname = if let Some(n) = subname.to_str() {
                Utf8Path::new(n)
            } else {
                continue;
            };
            if let (Some(stem), Some("commit")) = (subname.file_stem(), subname.extension()) {
                r.push(format!("{name}{stem}"));
            }
        }
    }

    Ok(r)
}

/// The implementation of `rpm-ostree ex deploy-from-self`, which writes
/// the container ostree commit to the host and deploys it, optionally rebooting.
pub(crate) fn deploy_from_self_entrypoint(args: Vec<String>) -> CxxResult<()> {
    use nix::sys::statvfs;
    let cancellable = gio::Cancellable::NONE;
    let opts = UpdateFromRunningOpts::parse_from(args);

    let sysroot = opts.target_root.join("sysroot");

    if statvfs::statvfs(sysroot.as_std_path())?
        .flags()
        .contains(statvfs::FsFlags::ST_RDONLY)
    {
        Command::new("mount")
            .args(["-o", "remount,rw", sysroot.as_str()])
            .run()?;
    }

    let src_repo_path = Utf8Path::new("/ostree/repo");
    // Just verify it can be opened for now...in the future ideally we'll use https://github.com/ostreedev/ostree/pull/2701
    let src_repo = ostree::Repo::open_at(libc::AT_FDCWD, src_repo_path.as_str(), cancellable)?;
    drop(src_repo);

    let encapsulated_commits = find_encapsulated_commits(src_repo_path)?;
    let commit = match encapsulated_commits.as_slice() {
        [] => {
            return Err("No encapsulated commit found in container"
                .to_string()
                .into())
        }
        [c] => {
            ostree::validate_checksum_string(c)?;
            c.as_str()
        }
        o => return Err(format!("Found {} commit objects, expected just one", o.len()).into()),
    };

    let target_repo = sysroot.join("ostree/repo");
    let target_repo = ostree::Repo::open_at(libc::AT_FDCWD, target_repo.as_str(), cancellable)?;

    {
        let flags = ostree::RepoPullFlags::MIRROR;
        let opts = glib::VariantDict::new(None);
        let refs = [commit];
        opts.insert("refs", &refs[..]);
        opts.insert("flags", flags.bits() as i32);
        let options = opts.to_variant();
        target_repo
            .pull_with_options(
                &format!("file://{src_repo_path}"),
                &options,
                None,
                cancellable,
            )
            .context("Pulling from embedded repo")?;
    }

    println!("Imported: {commit}");

    Command::new("chroot")
        .args([opts.target_root.as_str(), "rpm-ostree", "rebase", commit])
        .args(opts.reboot.then_some("--reboot"))
        .run()?;

    Ok(())
}
