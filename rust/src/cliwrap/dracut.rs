// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;

use crate::cliwrap::cliutil;

/// Primary entrypoint to running our wrapped `dracut` handling.
pub(crate) fn main(argv: &[&str]) -> Result<()> {
    // At least kdump.service runs dracut to generate a separate initramfs.
    // We need to continue supporting that.
    if crate::utils::running_in_systemd() {
        return cliutil::exec_real_binary("dracut", argv);
    }
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
