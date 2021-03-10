// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;

use crate::cliwrap::cliutil;

/// Primary entrypoint to running our wrapped `dracut` handling.
pub(crate) fn main(argv: &[&str]) -> Result<()> {
    eprintln!(
        "This system is rpm-ostree based; initramfs handling is
integrated with the underlying ostree transaction mechanism.
Use `rpm-ostree initramfs` to control client-side initramfs generation."
    );
    if !argv.is_empty() {
        Ok(cliutil::run_unprivileged(true, "dracut", argv)?)
    } else {
        std::process::exit(1);
    }
}
