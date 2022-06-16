//! CLI handler for `rpm-ostree usroverlay`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{Context, Result};
use clap::Command;
use std::os::unix::prelude::CommandExt;

/// Directly exec(ostree admin unlock) - does not return on success.
pub fn entrypoint(args: &[&str]) -> Result<()> {
    let cmd = cli_cmd();
    cmd.get_matches_from(args.iter().skip(1));

    let exec_err = std::process::Command::new("ostree")
        .args(&["admin", "unlock"])
        .exec();

    // This is only reached if the `exec()` above failed; otherwise
    // execution got transferred to `ostree` at that point.
    Err(exec_err).context("Failed to execute 'ostree admin unlock'")
}

/// CLI parser, handle --help and error on extra arguments.
fn cli_cmd() -> Command<'static> {
    Command::new("rpm-ostree usroverlay")
        .bin_name("rpm-ostree usroverlay")
        .long_version("")
        .long_about("Apply a transient overlayfs to /usr")
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_clap_cmd() {
        cli_cmd().debug_assert()
    }
}
