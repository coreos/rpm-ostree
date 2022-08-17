//! Common helpers for intercepted commands.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{Context, Result};
use cap_std::fs::Dir;

/// Directory for sysusers.d fragments.
pub(crate) static SYSUSERS_DIR: &str = "usr/lib/sysusers.d";

/// Create and open the `/usr/lib/sysusers.d` directory.
pub(crate) fn open_create_sysusers_dir(rootdir: &Dir) -> Result<Dir> {
    rootdir
        .create_dir_all(SYSUSERS_DIR)
        .with_context(|| format!("Creating '/{SYSUSERS_DIR}'"))?;
    let conf_dir = rootdir
        .open_dir(SYSUSERS_DIR)
        .with_context(|| format!("Opening '/{SYSUSERS_DIR}'"))?;
    Ok(conf_dir)
}
