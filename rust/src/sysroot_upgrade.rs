//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::{output_message, ContainerImageState};
use crate::ffi::{progress_begin_task, ExportedManifestDiff};
use anyhow::{Context, Result};
use ostree::glib;
use ostree_container::store::{
    ImageImporter, ImageProxyConfig, ImportProgress, ManifestLayerState, PrepareResult,
};
use ostree_container::OstreeImageReference;
use ostree_ext::container::{self as ostree_container, ManifestDiff};
use ostree_ext::ostree;
use tokio::runtime::Handle;
use tokio::sync::mpsc::Receiver;

impl From<Box<ostree_container::store::LayeredImageState>> for crate::ffi::ContainerImageState {
    fn from(s: Box<ostree_container::store::LayeredImageState>) -> crate::ffi::ContainerImageState {
        let version = ostree_container::version_for_config(&s.configuration)
            .map(ToOwned::to_owned)
            .unwrap_or_default();
        let cached_update_diff = s
            .cached_update
            .map(|c| {
                let diff = ManifestDiff::new(&s.manifest, &c.manifest);
                let version = ostree_container::version_for_config(&c.config)
                    .map(ToOwned::to_owned)
                    .unwrap_or_default();
                export_diff(&diff, version)
            })
            .unwrap_or_default();
        crate::ffi::ContainerImageState {
            base_commit: s.base_commit,
            merge_commit: s.merge_commit,
            image_digest: s.manifest_digest.to_string(),
            version,
            cached_update_diff,
        }
    }
}

/// Return a two-tuple where the second element is a two-tuple too:
/// (number of layers already stored, (number of layers to fetch, size of layers to fetch))
fn layer_counts<'a>(layers: impl Iterator<Item = &'a ManifestLayerState>) -> (u32, (u32, u64)) {
    layers.fold(
        (0u32, (0u32, 0u64)),
        |(stored, (n_to_fetch, size_to_fetch)), v| {
            if v.commit.is_some() {
                (stored + 1, (n_to_fetch, size_to_fetch))
            } else {
                (stored, (n_to_fetch + 1, size_to_fetch + v.layer().size()))
            }
        },
    )
}

// Output when we begin/end fetching a layer.  Ideally, we'd handle
// byte-level progress here too, but that requires some more sophisticated
// binding with the rpmostree-output.h via cxx.rs.
async fn layer_progress_print(mut r: Receiver<ImportProgress>, total_to_fetch: u32) {
    // This is just used to hold a reference to the task.
    #[allow(unused_variables, unused_assignments)]
    let mut task = None;
    let mut n_fetched = 0u64;
    while let Some(v) = r.recv().await {
        let mut msg = ostree_ext::cli::layer_progress_format(&v);
        msg.insert_str(0, &format!("[{n_fetched}/{total_to_fetch}] "));
        tracing::debug!("layer progress: {msg}");
        match v {
            ImportProgress::OstreeChunkStarted(_) => {
                assert!(task.is_none());
                task = Some(progress_begin_task(&msg));
            }
            ImportProgress::OstreeChunkCompleted(_) => {
                assert!(task.take().is_some());
                n_fetched += 1;
            }
            ImportProgress::DerivedLayerStarted(_) => {
                assert!(task.is_none());
                task = Some(progress_begin_task(&msg));
            }
            ImportProgress::DerivedLayerCompleted(_) => {
                assert!(task.take().is_some());
                n_fetched += 1;
            }
        }
    }
}

fn default_container_pull_config(imgref: &OstreeImageReference) -> Result<ImageProxyConfig> {
    let mut cfg = ImageProxyConfig::default();
    if imgref.imgref.transport == ostree_container::Transport::ContainerStorage {
        // Fetching from containers-storage, may require privileges to read files
        ostree_container::merge_default_container_proxy_opts_with_isolation(&mut cfg, None)?;
    } else {
        let isolation_systemd = crate::utils::running_in_systemd().then_some("rpm-ostree");
        let isolation_default = rustix::process::getuid().is_root().then_some("nobody");
        let isolation_user = isolation_systemd.or(isolation_default);
        ostree_container::merge_default_container_proxy_opts_with_isolation(
            &mut cfg,
            isolation_user,
        )?;
    }
    Ok(cfg)
}

/// Create a new image importer using our default configuration.
async fn new_importer(repo: &ostree::Repo, imgref: &OstreeImageReference) -> Result<ImageImporter> {
    let config = default_container_pull_config(imgref)?;
    let mut imp = ImageImporter::new(repo, imgref, config).await?;
    imp.require_bootable();
    Ok(imp)
}

async fn pull_container_async(
    repo: &ostree::Repo,
    imgref: &OstreeImageReference,
) -> Result<ContainerImageState> {
    output_message(&format!("Pulling manifest: {}", &imgref));
    let mut imp = new_importer(repo, imgref).await?;
    let layer_progress = imp.request_progress();
    let prep = match imp.prepare().await? {
        PrepareResult::AlreadyPresent(r) => return Ok(r.into()),
        PrepareResult::Ready(r) => r,
    };
    let digest = prep.manifest_digest.clone();
    output_message(&format!("Importing: {} (digest: {})", &imgref, &digest));
    let ostree_layers = prep.ostree_layers.iter().chain(&prep.ostree_commit_layer);
    let mut total_to_fetch = 0;
    let (stored, (n_to_fetch, size_to_fetch)) = layer_counts(ostree_layers);
    if stored > 0 {
        output_message(&format!("ostree chunk layers already present: {stored}"));
    }
    if n_to_fetch > 0 {
        let size = glib::format_size(size_to_fetch);
        output_message(&format!(
            "ostree chunk layers needed: {n_to_fetch} ({size})"
        ));
        total_to_fetch += n_to_fetch;
    }
    let (stored, (n_to_fetch, size_to_fetch)) = layer_counts(prep.layers.iter());
    if stored > 0 {
        output_message(&format!("custom layers already present: {stored}"));
    }
    if n_to_fetch > 0 {
        let size = glib::format_size(size_to_fetch);
        output_message(&format!("custom layers needed: {n_to_fetch} ({size})"));
        total_to_fetch += n_to_fetch;
    }
    let local = tokio::task::LocalSet::new();
    let import = local
        .run_until(async move {
            let _progress_printer =
                tokio::task::spawn_local(layer_progress_print(layer_progress, total_to_fetch));
            imp.import(prep).await
        })
        .await;
    // TODO log the discarded bits from import
    Ok(import?.into())
}

/// Import ostree commit in container image using ostree-rs-ext's API.
pub(crate) fn pull_container(
    repo: &crate::FFIOstreeRepo,
    cancellable: &crate::FFIGCancellable,
    imgref: &str,
) -> CxxResult<Box<ContainerImageState>> {
    let repo = &repo.glib_reborrow();
    let cancellable = cancellable.glib_reborrow();
    let imgref = &OstreeImageReference::try_from(imgref)?;

    crate::try_fail_point!("sysroot-upgrade::container-pull");

    let r = Handle::current().block_on(async {
        crate::utils::run_with_cancellable(
            async { pull_container_async(repo, imgref).await },
            &cancellable,
        )
        .await
    })?;
    Ok(Box::new(r))
}

/// Run a prune of container images that are not referenced by a deployment.
pub(crate) fn container_prune(
    sysroot: &crate::FFIOstreeSysroot,
) -> CxxResult<crate::ffi::PrunedContainerInfo> {
    let sysroot = &sysroot.glib_reborrow();
    tracing::debug!("Pruning container images");
    crate::try_fail_point!("sysroot-upgrade::container-prune");
    let sysroot = &ostree_ext::sysroot::SysrootLock::from_assumed_locked(sysroot);
    let images = ostree_container::deploy::remove_undeployed_images(sysroot)
        .context("Pruning images")?
        .len() as u32;
    let layers = ostree_container::store::gc_image_layers(&sysroot.repo())?;
    Ok(crate::ffi::PrunedContainerInfo { images, layers })
}

/// C++ wrapper for querying image state; requires a pulled image
pub(crate) fn query_container_image_commit(
    repo: &crate::FFIOstreeRepo,
    imgcommit: &str,
) -> CxxResult<Box<crate::ffi::ContainerImageState>> {
    let repo = &repo.glib_reborrow();
    let state = ostree_container::store::query_image_commit(repo, imgcommit)?;
    Ok(Box::new(state.into()))
}

/// Remove a refspec, which can be either an ostree branch or a container image.
pub(crate) fn purge_refspec(repo: &crate::FFIOstreeRepo, imgref: &str) -> CxxResult<()> {
    let repo = &repo.glib_reborrow();
    tracing::debug!("Purging {imgref}");
    if OstreeImageReference::try_from(imgref).is_ok() {
        // It's a container, so we will defer to the new model of pruning
        // container images that have no deployments in cleanup.
        tracing::debug!("No action needed");
    } else if ostree::validate_checksum_string(imgref).is_ok() {
        // Nothing to do here
    } else {
        match ostree::parse_refspec(imgref) {
            Ok((remote, ostreeref)) => {
                repo.set_ref_immediate(
                    remote.as_ref().map(|s| s.as_str()),
                    &ostreeref,
                    None,
                    ostree::gio::Cancellable::NONE,
                )?;
            }
            Err(e) => {
                // For historical reasons, we ignore errors here
                tracing::warn!("{e}");
            }
        }
    }
    Ok(())
}

/// Check for an updated manifest for the given container image reference, and return a diff.
pub(crate) fn check_container_update(
    repo: &crate::FFIOstreeRepo,
    cancellable: &crate::FFIGCancellable,
    imgref: &str,
) -> CxxResult<bool> {
    let repo = &repo.glib_reborrow();
    let cancellable = cancellable.glib_reborrow();
    let imgref = &OstreeImageReference::try_from(imgref)?;
    Handle::current()
        .block_on(async {
            crate::utils::run_with_cancellable(
                async { impl_check_container_update(repo, imgref).await },
                &cancellable,
            )
            .await
        })
        .map_err(Into::into)
}

/// Unfortunately we can't export external types into our C++ bridge, so manually copy things
/// to another copy of the struct.
fn export_diff(diff: &ManifestDiff, version: String) -> ExportedManifestDiff {
    ExportedManifestDiff {
        initialized: true,
        total: diff.total,
        total_size: diff.total_size,
        n_removed: diff.n_removed,
        removed_size: diff.removed_size,
        n_added: diff.n_added,
        added_size: diff.added_size,
        version,
    }
}

/// Implementation of fetching a container manifest diff.
async fn impl_check_container_update(
    repo: &ostree::Repo,
    imgref: &OstreeImageReference,
) -> Result<bool> {
    let mut imp = new_importer(repo, imgref).await?;
    let have_update = match imp.prepare().await? {
        PrepareResult::AlreadyPresent(_) => false,
        PrepareResult::Ready(_) => true,
    };
    Ok(have_update)
}

#[test]
fn test_container_manifest_diff() -> Result<()> {
    use ostree_ext::container::ManifestDiff;
    use ostree_ext::oci_spec::image::ImageManifest;
    let a: ImageManifest = serde_json::from_str(include_str!("../test/manifest1.json")).unwrap();
    let b: ImageManifest = serde_json::from_str(include_str!("../test/manifest2.json")).unwrap();
    let diff = ManifestDiff::new(&a, &b);

    let cmp_total = diff.total;
    let cmp_total_size = diff.total_size;
    let cmp_removed = diff.n_removed;
    let cmp_removed_size = diff.removed_size;
    let cmp_added = diff.n_added;
    let cmp_added_size = diff.added_size;

    assert_eq!(cmp_total, 51 as u64);
    assert_eq!(cmp_total_size, 697035490 as u64);
    assert_eq!(cmp_removed, 4 as u64);
    assert_eq!(cmp_removed_size, 170473141 as u64);
    assert_eq!(cmp_added, 4 as u64);
    assert_eq!(cmp_added_size, 170472856 as u64);

    Ok(())
}
