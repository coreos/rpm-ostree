//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::{output_message, ContainerImageState};
use anyhow::Result;
use ostree::glib;
use ostree_container::store::LayeredImageImporter;
use ostree_container::store::PrepareResult;
use ostree_container::OstreeImageReference;
use ostree_ext::container as ostree_container;
use ostree_ext::ostree;
use std::convert::TryFrom;
use std::pin::Pin;
use tokio::runtime::Handle;

impl From<ostree_container::store::LayeredImageState> for crate::ffi::ContainerImageState {
    fn from(s: ostree_container::store::LayeredImageState) -> crate::ffi::ContainerImageState {
        crate::ffi::ContainerImageState {
            base_commit: s.base_commit,
            merge_commit: s.merge_commit,
            is_layered: s.is_layered,
            image_digest: s.manifest_digest,
        }
    }
}

async fn pull_container_async(
    repo: &ostree::Repo,
    imgref: &OstreeImageReference,
) -> Result<ContainerImageState> {
    output_message(&format!("Pulling manifest: {}", &imgref));
    let mut imp = LayeredImageImporter::new(repo, imgref, Default::default()).await?;
    let prep = match imp.prepare().await? {
        PrepareResult::AlreadyPresent(r) => return Ok(r.into()),
        PrepareResult::Ready(r) => r,
    };
    let digest = prep.manifest_digest.clone();
    output_message(&format!("Importing: {} (digest: {})", &imgref, &digest));
    if prep.base_layer.commit.is_none() {
        let size = glib::format_size(prep.base_layer.size());
        output_message(&format!(
            "Downloading base layer: {} ({})",
            prep.base_layer.digest(),
            size
        ));
    } else {
        output_message(&format!("Using base: {}", prep.base_layer.digest()));
    }
    // TODO add nice download progress
    for layer in prep.layers.iter() {
        if layer.commit.is_some() {
            output_message(&format!("Using layer: {}", layer.digest()));
        } else {
            let size = glib::format_size(layer.size());
            output_message(&format!("Downloading layer: {} ({})", layer.digest(), size));
        }
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
