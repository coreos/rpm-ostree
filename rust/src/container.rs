//! CLI exposing `ostree-rs-ext container`

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::ffi::CStr;
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

use bootc_internal_utils::CommandRunExt;
use crate::cxxrsutil::FFIGObjectReWrap;
use crate::fsutil::{self, FileHelpers, ResolvedOstreePaths};
use crate::progress::progress_task;
use crate::CxxResult;

const COMPONENT_XATTR: &CStr = c"user.component";

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

    /// The encapsulated container format version; must be 1 or 2.
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

    /// Maps from component ID to metadata
    componentmeta: ObjectMetaSet,

    /// Component IDs encountered during filesystem walk for efficient lookup
    component_ids: HashSet<String>,

    /// Maps from object checksum to absolute filesystem path
    checksum_paths: BTreeMap<String, BTreeSet<Utf8PathBuf>>,

    /// Maps from absolute filesystem path to the package IDs that
    /// provide it
    path_packages: HashMap<Utf8PathBuf, BTreeSet<ContentID>>,

    /// Maps from absolute filesystem path to component IDs (for exclusive layers)
    path_components: HashMap<Utf8PathBuf, BTreeSet<ContentID>>,

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

// loop over checksum_paths (this is the entire list of files)
// check if there is a mapping for the file in the "explicit" mapping
// check if there is a mapping for the file in the package mapping
// otherwise put it in the unpackaged bucket
impl MappingBuilder {
    fn create_meta(&self) -> (ObjectMeta, ObjectMeta) {
        let mut package_content = ObjectMetaMap::default();
        let mut component_content = ObjectMetaMap::default();

        for (checksum, paths) in &self.checksum_paths {
            for path in paths {
                if let Some(component_ids) = self.path_components.get(path) {
                    if let Some(content_id) = component_ids.first() {
                        component_content.insert(checksum.clone(), content_id.clone());
                    }
                } else if let Some(package_ids) = self.path_packages.get(path) {
                    if let Some(content_id) = package_ids.first() {
                        package_content.insert(checksum.clone(), content_id.clone());
                    }
                } else {
                    package_content.insert(checksum.clone(), self.unpackaged_id.clone());
                }
            }
        }

        let package_meta = ObjectMeta {
            map: package_content,
            set: self.packagemeta.clone(),
        };

        let component_meta = ObjectMeta {
            map: component_content,
            set: self.componentmeta.clone(),
        };

        (package_meta, component_meta)
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

                // Try to read user.component xattr to identify component-based chunks
                if let Some(component_name) = get_user_component_xattr(&child)? {
                    let component_id = Rc::from(component_name.clone());

                    // Track component ID for later processing
                    state.component_ids.insert(component_name);

                    // Associate this path with the component
                    state
                        .path_components
                        .entry(path.clone())
                        .or_default()
                        .insert(Rc::clone(&component_id));
                };

                // Ensure there's a checksum -> path entry. If it was previously
                // accounted for by a package or component, this is essentially a no-op. If not,
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

/// Read the user.component xattr from a file path, returning None if not present
fn get_user_component_xattr(file: &ostree::RepoFile) -> std::io::Result<Option<String>> {
    let xattrs = match file.xattrs(gio::Cancellable::NONE) {
        Ok(xattrs) => xattrs,
        Err(_) => return Ok(None), // No xattrs available
    };

    let n = xattrs.n_children();
    for i in 0..n {
        let child = xattrs.child_value(i);
        let key = child.child_value(0);
        let key_bytes = key.data_as_bytes();

        if key_bytes == COMPONENT_XATTR.to_bytes_with_nul() {
            let value = child.child_value(1);
            let value = value.data_as_bytes();
            let value_str = String::from_utf8(value.to_vec())
                .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
            return Ok(Some(value_str));
        }
    }
    Ok(None)
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
        componentmeta: Default::default(),
        checksum_paths: Default::default(),
        path_packages: Default::default(),
        path_components: Default::default(),
        skip: Default::default(),
        component_ids: Default::default(),
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

    // Now that we've walked the filesystem, process component metadata
    for component_name in state.component_ids.iter() {
        let component_id = Rc::from(component_name.clone());
        let component_srcid = Rc::from(format!("component:{}", component_name));

        state.componentmeta.insert(ObjectSourceMeta {
            identifier: component_id,
            name: Rc::from(component_name.clone()),
            srcid: component_srcid,
            // Assume component content changes frequently
            change_time_offset: u32::MAX,
            change_frequency: u32::MAX,
        });
    }

    let src_pkgs: HashSet<_> = state.packagemeta.iter().map(|p| &p.srcid).collect();

    // Print out information about what we found
    println!(
        "{} objects in {} packages ({} source)",
        state.checksum_paths.len(),
        state.packagemeta.len(),
        src_pkgs.len(),
    );

    // Print component information
    if !state.componentmeta.is_empty() {
        println!("Found {} user component(s):", state.componentmeta.len());
        for component_meta in &state.componentmeta {
            // Count files belonging to this component
            let component_files = state
                .path_components
                .values()
                .filter(|ids| ids.contains(&component_meta.identifier))
                .count();
            println!("  - {} ({} files)", component_meta.name, component_files);
        }
    }

    println!("rpm size: {}", state.rpmsize);
    println!(
        "Earliest changed package: {} at {}",
        lowest_change_name,
        Utc.timestamp_opt(lowest_change_time.try_into().unwrap(), 0)
            .unwrap()
    );
    println!("Duplicates: {}", state.duplicate_objects().count());
    println!("Multiple owners: {}", state.multiple_owners().count());

    // Create separate ObjectMeta for packages and exclusive components
    let (package_meta, component_meta) = state.create_meta();

    // Generate sized versions
    let package_meta_sized = ObjectMetaSized::compute_sizes(repo, package_meta)?;
    let component_meta_sized = if !state.componentmeta.is_empty() {
        Some(ObjectMetaSized::compute_sizes(repo, component_meta)?)
    } else {
        None
    };
    if let Some(v) = opt.write_contentmeta_json {
        let v = v.strip_prefix("/").unwrap_or(&v);
        let root = Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
        root.atomic_replace_with(v, |w| {
            serde_json::to_writer(w, &package_meta_sized.sizes).map_err(anyhow::Error::msg)
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
    opts.package_contentmeta = Some(&package_meta_sized);
    // Set exclusive component metadata if any were found
    if let Some(ref component_meta) = component_meta_sized {
        println!(
            "Setting exclusive component metadata with {} component(s)",
            state.componentmeta.len()
        );
        opts.specific_contentmeta = Some(component_meta);
    }
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
    if opt.format_version >= 2 {
        opts.tar_create_parent_dirs = true;
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
            .run_capture_stderr()?;
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
        .run_capture_stderr()?;

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::{BTreeSet, HashMap, HashSet};

    #[test]
    fn test_mapping_builder_create_package_meta() {
        let mut builder = MappingBuilder {
            unpackaged_id: Rc::from("unpackaged"),
            packagemeta: ObjectMetaSet::new(),
            componentmeta: ObjectMetaSet::new(),
            checksum_paths: BTreeMap::new(),
            path_packages: HashMap::new(),
            path_components: HashMap::new(),
            skip: HashSet::new(),
            component_ids: HashSet::new(),
            rpmsize: 0,
        };

        // Add a package
        let pkg_id = Rc::from("test-package");
        builder.packagemeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(&pkg_id),
            name: Rc::from("Test Package"),
            srcid: Rc::from("test-package-1.0-1.fc39"),
            change_time_offset: 100,
            change_frequency: 50,
        });

        // Add a component
        let comp_id = Rc::from("test-component");
        builder.componentmeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(&comp_id),
            name: Rc::from("Test Component"),
            srcid: Rc::from("component:test-component"),
            change_time_offset: 200,
            change_frequency: 100,
        });

        // Add paths and checksums
        let path1 = Utf8PathBuf::from("/usr/bin/test");
        let path2 = Utf8PathBuf::from("/usr/share/component-file");
        let checksum1 = "abc123".to_string();
        let checksum2 = "def456".to_string();

        // Associate path1 with package
        builder.path_packages.insert(path1.clone(), {
            let mut set = BTreeSet::new();
            set.insert(Rc::clone(&pkg_id));
            set
        });

        // Associate path2 with component
        builder.path_components.insert(path2.clone(), {
            let mut set = BTreeSet::new();
            set.insert(Rc::clone(&comp_id));
            set
        });

        // Add checksums
        builder.checksum_paths.insert(checksum1.clone(), {
            let mut set = BTreeSet::new();
            set.insert(path1);
            set
        });
        builder.checksum_paths.insert(checksum2.clone(), {
            let mut set = BTreeSet::new();
            set.insert(path2);
            set
        });

        let (package_meta, _component_meta) = builder.create_meta();

        assert_eq!(package_meta.map.len(), 1);
        assert!(package_meta.map.contains_key(&checksum1));
        assert_eq!(package_meta.map[&checksum1], pkg_id);

        assert_eq!(package_meta.set.len(), 1);
        assert_eq!(package_meta.set.iter().next().unwrap().identifier, pkg_id);
    }

    #[test]
    fn test_mapping_builder_create_component_meta() {
        let mut builder = MappingBuilder {
            unpackaged_id: Rc::from("unpackaged"),
            packagemeta: ObjectMetaSet::new(),
            componentmeta: ObjectMetaSet::new(),
            checksum_paths: BTreeMap::new(),
            path_packages: HashMap::new(),
            path_components: HashMap::new(),
            skip: HashSet::new(),
            component_ids: HashSet::new(),
            rpmsize: 0,
        };

        // Add a package
        let pkg_id = Rc::from("test-package");
        builder.packagemeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(&pkg_id),
            name: Rc::from("Test Package"),
            srcid: Rc::from("test-package-1.0-1.fc39"),
            change_time_offset: 100,
            change_frequency: 50,
        });

        // Add a component
        let comp_id = Rc::from("test-component");
        builder.componentmeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(&comp_id),
            name: Rc::from("Test Component"),
            srcid: Rc::from("component:test-component"),
            change_time_offset: 200,
            change_frequency: 100,
        });

        // Add paths and checksums
        let path1 = Utf8PathBuf::from("/usr/bin/test");
        let path2 = Utf8PathBuf::from("/usr/share/component-file");
        let checksum1 = "abc123".to_string();
        let checksum2 = "def456".to_string();

        // Associate path1 with package
        builder.path_packages.insert(path1.clone(), {
            let mut set = BTreeSet::new();
            set.insert(Rc::clone(&pkg_id));
            set
        });

        // Associate path2 with component
        builder.path_components.insert(path2.clone(), {
            let mut set = BTreeSet::new();
            set.insert(Rc::clone(&comp_id));
            set
        });

        // Add checksums
        builder.checksum_paths.insert(checksum1.clone(), {
            let mut set = BTreeSet::new();
            set.insert(path1);
            set
        });
        builder.checksum_paths.insert(checksum2.clone(), {
            let mut set = BTreeSet::new();
            set.insert(path2);
            set
        });

        let (_package_meta, component_meta) = builder.create_meta();

        // Component meta should contain both checksums now
        // checksum2 maps to comp_id (via path_components)
        // checksum1 falls back to unpackaged_id (not in path_components)
        assert_eq!(component_meta.map.len(), 1);
        assert!(component_meta.map.contains_key(&checksum2));

        // Should have component metadata but not package metadata
        assert_eq!(component_meta.set.len(), 1);
        assert_eq!(
            component_meta.set.iter().next().unwrap().identifier,
            comp_id
        );
    }

    #[test]
    fn test_mapping_builder_mixed_content() {
        let mut builder = MappingBuilder {
            unpackaged_id: Rc::from("unpackaged"),
            packagemeta: ObjectMetaSet::new(),
            componentmeta: ObjectMetaSet::new(),
            checksum_paths: BTreeMap::new(),
            path_packages: HashMap::new(),
            path_components: HashMap::new(),
            skip: HashSet::new(),
            component_ids: HashSet::new(),
            rpmsize: 0,
        };

        // Add package and component metadata
        let pkg_id = Rc::from("test-package");
        builder.packagemeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(&pkg_id),
            name: Rc::from("Test Package"),
            srcid: Rc::from("test-package-1.0-1.fc39"),
            change_time_offset: 100,
            change_frequency: 50,
        });

        let comp_id = Rc::from("test-component");
        builder.componentmeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(&comp_id),
            name: Rc::from("Test Component"),
            srcid: Rc::from("component:test-component"),
            change_time_offset: 200,
            change_frequency: 100,
        });

        // Add unpackaged content
        let unpackaged_path = Utf8PathBuf::from("/usr/share/unpackaged");
        let unpackaged_checksum = "unpack123".to_string();
        builder.checksum_paths.insert(unpackaged_checksum.clone(), {
            let mut set = BTreeSet::new();
            set.insert(unpackaged_path);
            set
        });

        let (package_meta, _component_meta) = builder.create_meta();

        // Package meta should contain unpackaged content
        assert_eq!(package_meta.map.len(), 1);
        assert!(package_meta.map.contains_key(&unpackaged_checksum));
        assert_eq!(
            package_meta.map[&unpackaged_checksum],
            builder.unpackaged_id
        );
    }

    #[test]
    fn test_multiple_components() {
        let mut builder = MappingBuilder {
            unpackaged_id: Rc::from("unpackaged"),
            packagemeta: ObjectMetaSet::new(),
            componentmeta: ObjectMetaSet::new(),
            checksum_paths: BTreeMap::new(),
            path_packages: HashMap::new(),
            path_components: HashMap::new(),
            skip: HashSet::new(),
            component_ids: HashSet::new(),
            rpmsize: 0,
        };

        // Add multiple components
        let comp1_id = Rc::from("component1");
        let comp2_id = Rc::from("component2");

        builder.componentmeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(&comp1_id),
            name: Rc::from("Component 1"),
            srcid: Rc::from("component:component1"),
            change_time_offset: u32::MAX,
            change_frequency: u32::MAX,
        });

        builder.componentmeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(&comp2_id),
            name: Rc::from("Component 2"),
            srcid: Rc::from("component:component2"),
            change_time_offset: u32::MAX,
            change_frequency: u32::MAX,
        });

        // Add files for each component
        let path1 = Utf8PathBuf::from("/usr/share/comp1/file");
        let path2 = Utf8PathBuf::from("/usr/share/comp2/file");
        let checksum1 = "comp1_123".to_string();
        let checksum2 = "comp2_456".to_string();

        builder.path_components.insert(path1.clone(), {
            let mut set = BTreeSet::new();
            set.insert(Rc::clone(&comp1_id));
            set
        });

        builder.path_components.insert(path2.clone(), {
            let mut set = BTreeSet::new();
            set.insert(Rc::clone(&comp2_id));
            set
        });

        builder.checksum_paths.insert(checksum1.clone(), {
            let mut set = BTreeSet::new();
            set.insert(path1);
            set
        });

        builder.checksum_paths.insert(checksum2.clone(), {
            let mut set = BTreeSet::new();
            set.insert(path2);
            set
        });

        let (_package_meta, component_meta) = builder.create_meta();

        // Should contain both components
        assert_eq!(component_meta.map.len(), 2);
        assert!(component_meta.map.contains_key(&checksum1));
        assert!(component_meta.map.contains_key(&checksum2));
        assert_eq!(component_meta.map[&checksum1], comp1_id);
        assert_eq!(component_meta.map[&checksum2], comp2_id);
        assert_eq!(component_meta.set.len(), 2);
    }

    #[test]
    fn test_get_user_component_xattr() -> Result<(), Box<dyn std::error::Error>> {
        use std::fs;
        use std::os::fd::AsRawFd;
        use tempfile::TempDir;

        // Create temporary directory for our test repo
        let temp_dir = TempDir::new()?;
        let repo_path = temp_dir.path().join("repo");

        // Create OSTree repository using Rust API
        let repo = ostree::Repo::create_at(
            libc::AT_FDCWD,
            repo_path.to_str().unwrap(),
            ostree::RepoMode::Archive,
            None,
            gio::Cancellable::NONE,
        )?;

        // Create temporary work directory with test file
        let work_dir = temp_dir.path().join("work");
        fs::create_dir_all(&work_dir)?;
        let test_file_path = work_dir.join("test-file");
        fs::write(&test_file_path, "test content")?;

        // Create a mutable tree
        let mtree = ostree::MutableTree::new();

        // Create commit modifier with xattr callback
        let modifier = ostree::RepoCommitModifier::new(
            ostree::RepoCommitModifierFlags::NONE,
            None, // commit filter
        );

        // Create xattr callback that adds user.component xattr to our test file
        let xattr_callback =
            move |_repo: &ostree::Repo, path: &str, _file_info: &gio::FileInfo| -> glib::Variant {
                if path == "/test-file" {
                    // Create the xattr data using byte arrays that glib can understand
                    // OSTree expects xattrs in format: a(ayay) - array of (name, value) byte array pairs

                    // Create xattr tuples as (name, value) pairs where both are byte arrays
                    // The function expects the name to be "user.component\0" and value to be "test-component"
                    let xattr_data = vec![(&b"user.component\0"[..], &b"test-component"[..])];

                    // Convert to glib::Variant
                    glib::Variant::from(xattr_data)
                } else {
                    // Return empty xattr array for other files
                    let empty_xattrs: Vec<(&[u8], &[u8])> = vec![];
                    glib::Variant::from(empty_xattrs)
                }
            };

        modifier.set_xattr_callback(xattr_callback);

        // Write directory to mutable tree using OSTree API
        let work_dir_fd =
            cap_std::fs::Dir::open_ambient_dir(&work_dir, cap_std::ambient_authority())?;
        repo.write_dfd_to_mtree(
            work_dir_fd.as_raw_fd(),
            ".",
            &mtree,
            Some(&modifier),
            gio::Cancellable::NONE,
        )?;

        // Write mutable tree to get root directory
        let root = repo.write_mtree(&mtree, gio::Cancellable::NONE)?;

        // Convert gio::File to ostree::RepoFile for write_commit
        let root_repo_file = root.downcast::<ostree::RepoFile>().unwrap();

        // Create metadata for the commit
        let metadata = glib::VariantDict::new(None);
        metadata.insert("test", "xattr-test");
        let metadata_variant = metadata.to_variant();

        // Write commit using OSTree API
        let commit_checksum = repo.write_commit(
            None,                           // parent
            Some("Test commit"),            // subject
            Some("Test commit with xattr"), // body
            Some(&metadata_variant),        // metadata
            &root_repo_file,                // root
            gio::Cancellable::NONE,
        )?;

        // Read back the commit to get our RepoFile
        let (commit_root, _) = repo.read_commit(&commit_checksum, gio::Cancellable::NONE)?;
        let test_file = commit_root.child("test-file");
        let repo_file = test_file.downcast::<ostree::RepoFile>().unwrap();

        // Test our function with the file that should have the xattr
        let result = get_user_component_xattr(&repo_file)?;

        // Verify the function found the xattr and contains our test component
        assert!(result.is_some());
        let result_str = result.unwrap();
        assert!(result_str.contains("test-component"));

        // Test with a file that should have no component xattr (root directory)
        let root_repo_file = commit_root.downcast::<ostree::RepoFile>().unwrap();
        let no_result = get_user_component_xattr(&root_repo_file)?;
        assert_eq!(no_result, None);
        Ok(())
    }
}
