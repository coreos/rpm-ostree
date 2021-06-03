//! CLI exposing `ostree-rs-ext container`

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{Context, Result};

/// Main entrypoint for container
pub fn entrypoint(args: &[&str]) -> Result<()> {
    // Right now we're only exporting the `container` bits, not tar.  So inject that argument.
    // And we also need to skip the main arg and the `ex-container` arg.
    let args = ["rpm-ostree", "container"]
        .iter()
        .chain(args.iter().skip(2));
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("Failed to build tokio runtime")?
        .block_on(async { ostree_ext::cli::run_from_iter(args).await })?;
    Ok(())
}
