//! CLI handler for `rpm-ostree usroverlay`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{Context, Result};
use std::os::unix::prelude::CommandExt;

/// Directly exec(ostree admin unlock) - does not return on success.
pub fn usroverlay_entrypoint(args: &Vec<String>) -> Result<()> {
    let exec_err = std::process::Command::new("ostree")
        .args(&["admin", "unlock"])
        .args(args.into_iter().skip(1))
        .exec();
    // This is only reached if the `exec()` above failed; otherwise
    // execution got transferred to `ostree` at that point.
    Err(exec_err).context("Failed to execute 'ostree admin unlock'")
}
