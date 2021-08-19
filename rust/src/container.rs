//! CLI exposing `ostree-rs-ext container`

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;

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
