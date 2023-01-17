//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::{output_message, ContainerImageState};
use anyhow::Result;
use ostree::glib;
use ostree::prelude::*;
use ostree_container::store::{
    ImageImporter, ImageProxyConfig, ImportProgress, ManifestLayerState, PrepareResult,
};
use ostree_container::OstreeImageReference;
use ostree_ext::container as ostree_container;
use ostree_ext::ostree;
use tokio::runtime::Handle;
use tokio::sync::mpsc::Receiver;

impl From<Box<ostree_container::store::LayeredImageState>> for crate::ffi::ContainerImageState {
    fn from(s: Box<ostree_container::store::LayeredImageState>) -> crate::ffi::ContainerImageState {
        let version = s
            .configuration
            .as_ref()
            .and_then(|c| ostree_container::version_for_config(c))
            .map(ToOwned::to_owned)
            .unwrap_or_default();
        crate::ffi::ContainerImageState {
            base_commit: s.base_commit,
            merge_commit: s.merge_commit,
            is_layered: s.is_layered,
            image_digest: s.manifest_digest,
            version,
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
                (stored, (n_to_fetch + 1, size_to_fetch + v.size()))
            }
        },
    )
}

async fn layer_progress_print(mut r: Receiver<ImportProgress>) {
    while let Some(v) = r.recv().await {
        let msg = ostree_ext::cli::layer_progress_format(&v);
        output_message(&msg);
    }
}

fn default_container_pull_config() -> Result<ImageProxyConfig> {
    let mut cfg = ImageProxyConfig::default();
    let isolation_systemd = crate::utils::running_in_systemd().then(|| "rpm-ostree");
    let isolation_default = cap_std_ext::rustix::process::getuid()
        .is_root()
        .then(|| "nobody");
    let isolation_user = isolation_systemd.or(isolation_default);
    ostree_container::merge_default_container_proxy_opts_with_isolation(&mut cfg, isolation_user)?;
    Ok(cfg)
}

async fn pull_container_async(
    repo: &ostree::Repo,
    imgref: &OstreeImageReference,
) -> Result<ContainerImageState> {
    output_message(&format!("Pulling manifest: {}", &imgref));
    let config = default_container_pull_config()?;
    let mut imp = ImageImporter::new(repo, imgref, config).await?;
    let layer_progress = imp.request_progress();
    let prep = match imp.prepare().await? {
        PrepareResult::AlreadyPresent(r) => return Ok(r.into()),
        PrepareResult::Ready(r) => r,
    };
    if prep.export_layout == ostree_container::ExportLayout::V0 {
        output_message(&format!("warning: pulled image is using deprecated v0 format; support will be dropped in a future release"));
        std::thread::sleep(std::time::Duration::from_secs(5));
    }
    let progress_printer =
        tokio::task::spawn(async move { layer_progress_print(layer_progress).await });
    let digest = prep.manifest_digest.clone();
    output_message(&format!("Importing: {} (digest: {})", &imgref, &digest));
    let ostree_layers = prep
        .ostree_layers
        .iter()
        .chain(std::iter::once(&prep.ostree_commit_layer));
    let (stored, (n_to_fetch, size_to_fetch)) = layer_counts(ostree_layers);
    if stored > 0 || n_to_fetch > 0 {
        let size = glib::format_size(size_to_fetch);
        output_message(&format!(
            "ostree chunk layers stored: {stored} needed: {n_to_fetch} ({size})"
        ));
    }
    let (stored, (n_to_fetch, size_to_fetch)) = layer_counts(prep.layers.iter());
    if stored > 0 || n_to_fetch > 0 {
        let size = glib::format_size(size_to_fetch);
        output_message(&format!(
            "custom layers stored: {stored} needed: {n_to_fetch} ({size})"
        ));
    }
    let import = imp.import(prep).await;
    let _ = progress_printer.await;
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

pub(crate) fn layer_prune(
    repo: &ostree::Repo,
    cancellable: Option<&ostree::gio::Cancellable>,
) -> Result<()> {
    if let Some(c) = cancellable {
        c.set_error_if_cancelled()?;
    }
    tracing::debug!("pruning image layers");
    crate::try_fail_point!("sysroot-upgrade::layer-prune");
    let n_pruned = ostree_ext::container::store::gc_image_layers(repo)?;
    systemd::journal::print(6, &format!("Pruned container image layers: {n_pruned}"));
    Ok(())
}

/// Run a prune of container image layers.
pub(crate) fn container_prune(
    repo: &crate::FFIOstreeRepo,
    cancellable: &crate::FFIGCancellable,
) -> CxxResult<()> {
    layer_prune(&repo.glib_reborrow(), Some(&cancellable.glib_reborrow())).map_err(Into::into)
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
    if let Ok(cref) = OstreeImageReference::try_from(imgref) {
        // It's a container, use the ostree-ext APIs to prune it.
        ostree_container::store::remove_image(repo, &cref.imgref)?;
        layer_prune(repo, None)?;
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
