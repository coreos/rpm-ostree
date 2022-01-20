//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::{output_message, ContainerImageState};
use anyhow::Result;
use ostree::glib;
use ostree_container::store::ImageImporter;
use ostree_container::store::PrepareResult;
use ostree_container::OstreeImageReference;
use ostree_ext::container as ostree_container;
use ostree_ext::container::store::ManifestLayerState;
use ostree_ext::ostree;
use std::convert::TryFrom;
use std::pin::Pin;
use tokio::runtime::Handle;

impl From<Box<ostree_container::store::LayeredImageState>> for crate::ffi::ContainerImageState {
    fn from(s: Box<ostree_container::store::LayeredImageState>) -> crate::ffi::ContainerImageState {
        crate::ffi::ContainerImageState {
            base_commit: s.base_commit,
            merge_commit: s.merge_commit,
            is_layered: s.is_layered,
            image_digest: s.manifest_digest,
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

async fn pull_container_async(
    repo: &ostree::Repo,
    imgref: &OstreeImageReference,
) -> Result<ContainerImageState> {
    output_message(&format!("Pulling manifest: {}", &imgref));
    let mut imp = ImageImporter::new(repo, imgref, Default::default()).await?;
    let prep = match imp.prepare().await? {
        PrepareResult::AlreadyPresent(r) => return Ok(r.into()),
        PrepareResult::Ready(r) => r,
    };
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
    let import = imp.import(prep).await?;
    // TODO log the discarded bits from import
    Ok(import.into())
}

/// Import ostree commit in container image using ostree-rs-ext's API.
pub(crate) fn pull_container(
    mut repo: Pin<&mut crate::FFIOstreeRepo>,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
    imgref: &str,
) -> CxxResult<Box<ContainerImageState>> {
    let repo = &repo.gobj_wrap();
    let cancellable = cancellable.gobj_wrap();
    let imgref = &OstreeImageReference::try_from(imgref)?;

    let r = Handle::current().block_on(async {
        crate::utils::run_with_cancellable(
            async { pull_container_async(repo, imgref).await },
            &cancellable,
        )
        .await
    })?;
    Ok(Box::new(r))
}

/// C++ wrapper for querying image state; requires a pulled image
pub(crate) fn query_container_image(
    mut repo: Pin<&mut crate::FFIOstreeRepo>,
    imgref: &str,
) -> CxxResult<Box<crate::ffi::ContainerImageState>> {
    let repo = &repo.gobj_wrap();
    let imgref = &OstreeImageReference::try_from(imgref)?;
    let state = ostree_container::store::query_image(repo, imgref)?
        .ok_or_else(|| anyhow::anyhow!("Failed to find image {}", imgref))?;
    Ok(Box::new(state.into()))
}
