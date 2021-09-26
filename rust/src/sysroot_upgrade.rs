//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::{output_message, ContainerImport};
use anyhow::Result;
use ostree_container::store::LayeredImageImporter;
use ostree_container::store::PrepareResult;
use ostree_container::OstreeImageReference;
use ostree_ext::container as ostree_container;
use ostree_ext::ostree;
use std::convert::TryFrom;
use std::pin::Pin;
use tokio::runtime::Handle;

async fn pull_container_async(
    repo: &ostree::Repo,
    imgref: &OstreeImageReference,
    current_digest: Option<&str>,
) -> Result<ContainerImport> {
    let unchanged = || ContainerImport {
        changed: false,
        ostree_commit: "".to_string(),
        image_digest: "".to_string(),
    };
    output_message(&format!("Pulling manifest: {}", &imgref));
    match &imgref.sigverify {
        ostree_container::SignatureSource::OstreeRemote(_) => {
            let (_, digest) = ostree_container::fetch_manifest(&imgref).await?;
            if Some(digest.as_str()) == current_digest {
                return Ok(unchanged());
            }
            output_message(&format!("Importing: {} (digest: {})", &imgref, &digest));
            let i = ostree_container::import(repo, imgref, None).await?;
            Ok(ContainerImport {
                changed: true,
                ostree_commit: i.ostree_commit,
                image_digest: i.image_digest,
            })
        }
        _ => {
            let mut imp = LayeredImageImporter::new(repo, imgref).await?;
            let prep = match imp.prepare().await? {
                PrepareResult::AlreadyPresent(_) => return Ok(unchanged()),
                PrepareResult::Ready(r) => r,
            };
            let digest = prep.manifest_digest.clone();
            output_message(&format!("Importing: {} (digest: {})", &imgref, &digest));
            let import = imp.import(prep).await?;
            Ok(ContainerImport {
                changed: true,
                ostree_commit: import,
                image_digest: digest,
            })
        }
    }
}

/// Import ostree commit in container image using ostree-rs-ext's API.
pub(crate) fn pull_container(
    mut repo: Pin<&mut crate::FFIOstreeRepo>,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
    imgref: &str,
    current_digest: &str,
) -> CxxResult<Box<ContainerImport>> {
    let repo = &repo.gobj_wrap();
    let cancellable = cancellable.gobj_wrap();
    let current_digest = if current_digest.is_empty() {
        None
    } else {
        Some(current_digest)
    };
    let imgref = &OstreeImageReference::try_from(imgref)?;

    let r = Handle::current().block_on(async {
        crate::utils::run_with_cancellable(
            async { pull_container_async(repo, imgref, current_digest).await },
            &cancellable,
        )
        .await
    })?;
    Ok(Box::new(r))
}
