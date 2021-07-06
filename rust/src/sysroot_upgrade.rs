//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use anyhow::{Context, Result};
use std::convert::TryInto;
use std::pin::Pin;

/// Import ostree commit in container image using ostree-rs-ext's API.
pub fn import_container(
    mut sysroot: Pin<&mut crate::FFIOstreeSysroot>,
    imgref: String,
) -> CxxResult<String> {
    // TODO: take a GCancellable and monitor it, and drop the import task (which is how async cancellation works in Rust).
    let sysroot = &sysroot.gobj_wrap();
    let repo = &sysroot.get_repo(gio::NONE_CANCELLABLE)?;
    let imgref = imgref.as_str().try_into()?;
    let commit = build_runtime()?
        .block_on(async { ostree_ext::container::import(&repo, &imgref, None).await })?;
    Ok(commit.ostree_commit)
}

fn build_runtime() -> Result<tokio::runtime::Runtime> {
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("Failed to build tokio runtime")
}
