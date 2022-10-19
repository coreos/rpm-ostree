//! CLI exposing `ostree-rs-ext container`

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::collections::{BTreeMap, HashMap, HashSet};
use std::num::NonZeroU32;
use std::process::Command;
use std::rc::Rc;

use anyhow::Result;
use camino::{Utf8Path, Utf8PathBuf};
use cap_std::fs::Dir;
use cap_std_ext::cap_std;
use cap_std_ext::prelude::*;
use chrono::prelude::*;
use clap::Parser;
use ostree::glib;
use ostree_ext::chunking::ObjectMetaSized;
use ostree_ext::container::{Config, ExportLayout, ExportOpts, ImageReference};
use ostree_ext::objectsource::{
    ContentID, ObjectMeta, ObjectMetaMap, ObjectMetaSet, ObjectSourceMeta,
};
use ostree_ext::prelude::*;
use ostree_ext::{gio, ostree};

use crate::cxxrsutil::FFIGObjectReWrap;
use crate::progress::progress_task;
use crate::CxxResult;

/// Main entrypoint for container
pub async fn entrypoint(args: &[&str]) -> Result<i32> {
    // Right now we're only exporting the `container` bits, not tar.  So inject that argument.
    // And we also need to skip the main arg and the `ex-container` arg.
    let args = ["rpm-ostree", "container"]
        .iter()
        .chain(args.iter().skip(2));
    ostree_ext::cli::run_from_iter(args).await?;
    Ok(0)
}

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

    /// Propagate an OSTree commit metadata key to container label
    #[clap(name = "copymeta", long)]
    copy_meta_keys: Vec<String>,

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
}

#[derive(Debug)]
struct MappingBuilder {
    /// Maps from package ID to metadata
    packagemeta: ObjectMetaSet,
    /// Mapping from content object sha256 to package numeric ID
    content: ObjectMetaMap,
    /// Mapping from content object sha256 to package numeric ID
    duplicates: BTreeMap<String, Vec<ContentID>>,
    multi_provider: Vec<Utf8PathBuf>,

    unpackaged_id: Rc<str>,

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
}

impl From<MappingBuilder> for ObjectMeta {
    fn from(b: MappingBuilder) -> ObjectMeta {
        ObjectMeta {
            map: b.content,
            set: b.packagemeta,
        }
    }
}

/// Walk over the whole filesystem, and generate mappings from content object checksums
/// to the package that owns them.  
///
/// In the future, we could compute this much more efficiently by walking that
/// instead.  But this design is currently oriented towards accepting a single ostree
/// commit as input.
fn build_mapping_recurse(
    path: &mut Utf8PathBuf,
    dir: &gio::File,
    ts: &crate::ffi::RpmTs,
    state: &mut MappingBuilder,
) -> Result<()> {
    use std::collections::btree_map::Entry;
    let cancellable = gio::NONE_CANCELLABLE;
    let e = dir.enumerate_children(
        "standard::name,standard::type",
        gio::FileQueryInfoFlags::NOFOLLOW_SYMLINKS,
        cancellable,
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

                let mut pkgs = ts.packages_providing_file(path.as_str())?;
                // Let's be deterministic (but _unstable because we don't care about behavior of equal strings)
                pkgs.sort_unstable();
                // For now, we pick the alphabetically first package providing a file
                let mut pkgs = pkgs.into_iter();
                let pkgid = pkgs
                    .next()
                    .map(|v| -> Result<_> {
                        // Safety: we should have the package in metadata
                        let meta = state.packagemeta.get(v.as_str()).ok_or_else(|| {
                            anyhow::anyhow!("Internal error: missing pkgmeta for {}", &v)
                        })?;
                        Ok(Rc::clone(&meta.identifier))
                    })
                    .transpose()?
                    .unwrap_or_else(|| Rc::clone(&state.unpackaged_id));
                // Track cases of duplicate owners
                match pkgs.len() {
                    0 => {}
                    _ => {
                        state.multi_provider.push(path.clone());
                    }
                }

                let checksum = child.checksum().unwrap().to_string();
                match state.content.entry(checksum) {
                    Entry::Vacant(v) => {
                        v.insert(pkgid);
                    }
                    Entry::Occupied(_) => {
                        let checksum = child.checksum().unwrap().to_string();
                        let v = state.duplicates.entry(checksum).or_default();
                        v.push(pkgid);
                    }
                }
            }
            gio::FileType::Directory => {
                build_mapping_recurse(path, &child, ts, state)?;
            }
            o => anyhow::bail!("Unhandled file type: {}", o),
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

/// Like `ostree container encapsulate`, but uses chunks derived from package data.
pub fn container_encapsulate(args: Vec<String>) -> Result<()> {
    let args = args.iter().skip(1).map(|s| s.as_str());
    let opt = ContainerEncapsulateOpts::parse_from(args);
    let repo = &ostree_ext::cli::parse_repo(&opt.repo)?;
    let (root, rev) = repo.read_commit(opt.ostree_ref.as_str(), gio::NONE_CANCELLABLE)?;
    let pkglist = progress_task("Reading packages", || -> Result<_> {
        let cancellable = gio::Cancellable::new();
        let r = crate::ffi::package_variant_list_for_commit(
            repo.reborrow_cxx(),
            rev.as_str(),
            cancellable.reborrow_cxx(),
        )?;
        let r: glib::Variant = unsafe { glib::translate::from_glib_full(r as *mut _) };
        Ok(r)
    })?;

    // Open the RPM database for this commit.
    let q = crate::ffi::rpmts_for_commit(repo.reborrow_cxx(), rev.as_str())?;

    let mut state = MappingBuilder {
        unpackaged_id: Rc::from(MappingBuilder::UNPACKAGED_ID),
        packagemeta: Default::default(),
        content: Default::default(),
        duplicates: Default::default(),
        multi_provider: Default::default(),
        skip: Default::default(),
        rpmsize: Default::default(),
    };
    // Insert metadata for unpakaged content.
    state.packagemeta.insert(ObjectSourceMeta {
        identifier: Rc::clone(&state.unpackaged_id),
        name: Rc::clone(&state.unpackaged_id),
        srcid: Rc::clone(&state.unpackaged_id),
        // Assume that content in here changes frequently.
        change_time_offset: u32::MAX,
    });

    let mut lowest_change_time = None;
    let mut package_meta = HashMap::new();
    for pkg in pkglist.iter() {
        let name = pkg.child_value(0);
        let name = name.str().unwrap();
        let nevra = Rc::from(gv_nevra_to_string(&pkg).into_boxed_str());
        let pkgmeta = q.package_meta(name)?;
        let buildtime = pkgmeta.buildtime();
        if let Some((lowid, lowtime)) = lowest_change_time.as_mut() {
            if *lowtime > buildtime {
                *lowid = Rc::clone(&nevra);
                *lowtime = buildtime;
            }
        } else {
            lowest_change_time = Some((Rc::clone(&nevra), pkgmeta.buildtime()))
        }
        state.rpmsize += pkgmeta.size();
        package_meta.insert(nevra, pkgmeta);
    }

    // SAFETY: There must be at least one package.
    let (lowest_change_name, lowest_change_time) =
        lowest_change_time.expect("Failed to find any packages");
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
        state.packagemeta.insert(ObjectSourceMeta {
            identifier: Rc::clone(nevra),
            name: Rc::clone(nevra),
            srcid: Rc::from(pkgmeta.src_pkg().to_str().unwrap()),
            change_time_offset,
        });
    }

    let kernel_dir = ostree_ext::bootabletree::find_kernel_dir(&root, gio::NONE_CANCELLABLE)?;
    if let Some(kernel_dir) = kernel_dir {
        let kernel_ver: Utf8PathBuf = kernel_dir.basename().unwrap().try_into()?;
        let initramfs = kernel_dir.child("initramfs.img");
        if initramfs.query_exists(gio::NONE_CANCELLABLE) {
            let path: Utf8PathBuf = initramfs.path().unwrap().try_into()?;
            let initramfs = initramfs.downcast_ref::<ostree::RepoFile>().unwrap();
            let checksum = initramfs.checksum().unwrap();
            let name = format!("initramfs (kernel {})", kernel_ver).into_boxed_str();
            let name = Rc::from(name);
            state.content.insert(checksum.to_string(), Rc::clone(&name));
            state.packagemeta.insert(ObjectSourceMeta {
                identifier: Rc::clone(&name),
                name: Rc::clone(&name),
                srcid: Rc::clone(&name),
                change_time_offset: u32::MAX,
            });
            state.skip.insert(path);
        }
    }

    let rpmdb = root.resolve_relative_path(crate::composepost::RPMOSTREE_RPMDB_LOCATION);
    if rpmdb.query_exists(gio::NONE_CANCELLABLE) {
        // TODO add mapping for rpmdb
    }

    // Walk the filesystem
    progress_task("Building package mapping", || {
        build_mapping_recurse(&mut Utf8PathBuf::from("/"), &root, &q, &mut state)
    })?;

    let src_pkgs: HashSet<_> = state.packagemeta.iter().map(|p| &p.srcid).collect();

    // Print out information about what we found
    println!(
        "{} objects in {} packages ({} source)",
        state.content.len(),
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
    println!("{} duplicates", state.duplicates.len());
    if !state.multi_provider.is_empty() {
        println!("Multiple owners:");
        for v in state.multi_provider.iter() {
            println!("  {}", v);
        }
    }

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

    let mut copy_meta_keys = opt.copy_meta_keys;
    // Default to copying the input hash to support cheap change detection
    copy_meta_keys.push("rpmostree.inputhash".to_string());

    let config = Config {
        labels: Some(labels),
        cmd: opt.cmd,
    };
    let format = match opt.format_version {
        0 => ExportLayout::V0,
        1 => ExportLayout::V1,
        n => anyhow::bail!("Invalid format version {n}"),
    };
    let opts = ExportOpts {
        copy_meta_keys,
        max_layers: opt.max_layers,
        format,
        ..Default::default()
    };
    let handle = tokio::runtime::Handle::current();
    let digest = progress_task("Generating container image", || {
        handle.block_on(async {
            ostree_ext::container::encapsulate(
                repo,
                rev.as_str(),
                &config,
                Some(opts),
                Some(meta),
                &opt.imgref,
            )
            .await
        })
    })?;
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
fn find_encapsulated_commits(repo: &Utf8Path) -> Result<Vec<String>> {
    let objects = Dir::open_ambient_dir(&repo.join("objects"), cap_std::ambient_authority())?;
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
    let cancellable = gio::NONE_CANCELLABLE;
    let opts = UpdateFromRunningOpts::parse_from(args);

    let sysroot = opts.target_root.join("sysroot");

    if statvfs::statvfs(sysroot.as_std_path())?
        .flags()
        .contains(statvfs::FsFlags::ST_RDONLY)
    {
        let status = Command::new("mount")
            .args(["-o", "remount,rw", sysroot.as_str()])
            .status()?;
        if !status.success() {
            return Err(format!("Failed to remount /sysroot writable: {:?}", status).into());
        }
    }

    let src_repo_path = Utf8Path::new("/ostree/repo");
    // Just verify it can be opened for now...in the future ideally we'll use https://github.com/ostreedev/ostree/pull/2701
    let src_repo = ostree::Repo::open_at(libc::AT_FDCWD, src_repo_path.as_str(), cancellable)?;
    drop(src_repo);

    let encapsulated_commits = find_encapsulated_commits(src_repo_path)?;
    let commit = match encapsulated_commits.as_slice() {
        [] => return Err(format!("No encapsulated commit found in container").into()),
        [c] => c.as_str(),
        o => return Err(format!("Found {} commit objects, expected just one", o.len()).into()),
    };

    let target_repo = sysroot.join("ostree/repo");
    let target_repo = ostree::Repo::open_at(libc::AT_FDCWD, target_repo.as_str(), cancellable)?;

    {
        let flags = ostree::RepoPullFlags::MIRROR;
        let opts = glib::VariantDict::new(None);
        let refs = [commit];
        opts.insert("refs", &&refs[..]);
        opts.insert("flags", &(flags.bits() as i32));
        let options = opts.to_variant();
        target_repo.pull_with_options(
            &format!("file://{src_repo_path}"),
            &options,
            None,
            cancellable,
        )?;
    }

    println!("Imported: {commit}");

    let status = Command::new("chroot")
        .args(&[opts.target_root.as_str(), "rpm-ostree", "rebase", commit])
        .args(opts.reboot.then(|| "--reboot"))
        .status()?;
    if !status.success() {
        return Err(format!("Failed to deploy commit: {:?}", status).into());
    }

    Ok(())
}
