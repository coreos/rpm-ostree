//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffi::ContainerImport;
use std::convert::TryInto;
use std::pin::Pin;
use tokio::runtime::Handle;

/// Import ostree commit in container image using ostree-rs-ext's API.
pub(crate) fn import_container(
    mut repo: Pin<&mut crate::FFIOstreeRepo>,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
    imgref: String,
) -> CxxResult<Box<ContainerImport>> {
    let repo = repo.gobj_wrap();
    let cancellable = cancellable.gobj_wrap();
    let imgref = imgref.as_str().try_into()?;

    let imported = Handle::current().block_on(async {
        crate::utils::run_with_cancellable(
            async { ostree_ext::container::import(&repo, &imgref, None).await },
            &cancellable,
        )
        .await
    })?;
    Ok(Box::new(ContainerImport {
        ostree_commit: imported.ostree_commit,
        image_digest: imported.image_digest,
    }))
}

/// Fetch the image digest for `imgref` using ostree-rs-ext's API.
pub(crate) fn fetch_digest(
    imgref: String,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
) -> CxxResult<String> {
    let imgref = imgref.as_str().try_into()?;
    let cancellable = cancellable.gobj_wrap();

    let digest = Handle::current().block_on(async {
        crate::utils::run_with_cancellable(
            async { ostree_ext::container::fetch_manifest_info(&imgref).await },
            &cancellable,
        )
        .await
    })?;
    Ok(digest.manifest_digest)
}
