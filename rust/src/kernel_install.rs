//! Integration with the systemd-owned /sbin/kernel-install tooling.
//!
//! Note that there's two different phases of kernel handling:
//!
//! - build time
//! - deployment time (owned by ostree)
//!
//! This code is wholly concerned with "build time" today. The
//! "deployment time" logic is owned entirely by ostree.
//!

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::Path;
use std::process::Command;

use anyhow::{Context, Result};
use cap_std::fs::Dir;
use cap_std_ext::cap_std;
use cap_std_ext::dirext::CapStdExtDirExt;
use fn_error_context::context;

/// Parsed by kernel-install and set in the environment
const LAYOUT_VAR: &str = "KERNEL_INSTALL_LAYOUT";
/// The value we expect to find for layout
const LAYOUT_OSTREE: &str = "ostree";
/// What we should emit to skip further processing
const SKIP: u8 = 77;
/// The path to the kernel modules
const MODULES: &str = "usr/lib/modules";
/// The default name for the initramfs.
const INITRAMFS: &str = "initramfs.img";
/// The path to the instal.conf that sets layout.
const KERNEL_INSTALL_CONF: &str = "/usr/lib/kernel/install.conf";

#[context("Verifying kernel-install layout file")]
pub fn is_ostree_layout() -> Result<bool> {
    let install_conf = Path::new(KERNEL_INSTALL_CONF);
    if !install_conf.is_file() {
        tracing::debug!("can not read /usr/lib/kernel/install.conf");
        return Ok(false);
    }
    let buff = BufReader::new(
        File::open(install_conf).context("Failed to open /usr/lib/kernel/install.conf")?,
    );
    // Check for "layout=ostree" in the file
    for line in buff.lines() {
        let line = line.context("Failed to read line")?;
        if line.trim() == "layout=ostree" {
            return Ok(true);
        }
    }
    Ok(false)
}

#[context("Adding kernel")]
fn add(root: &Dir, argv: &[&str]) -> Result<()> {
    let mut argv_it = argv.iter().copied();
    let Some(kver) = argv_it.next() else {
        anyhow::bail!("No kernel version provided");
    };
    tracing::debug!("Installing kernel kver={kver}");
    println!("Generating initramfs");
    crate::initramfs::run_dracut(root, &kver)?;
    println!("Running depmod");
    let st = Command::new("depmod")
        .args(["-a", kver])
        .status()
        .context("Invoking depmod")?;
    if !st.success() {
        anyhow::bail!("Failed to run depmod: {st:?}");
    }
    Ok(())
}

#[context("Removing kernel")]
fn remove(root: &Dir, kver: &str) -> Result<()> {
    tracing::debug!("Removing kernel kver={kver}");
    let kdir = format!("{MODULES}/{kver}");
    let Some(kernel_dir) = root.open_dir_optional(&kdir)? else {
        return Ok(());
    };
    // We generate the initramfs, so remove it if it exists.
    kernel_dir.remove_file_optional(INITRAMFS)?;
    Ok(())
}

/// Primary entrypoint to `/usr/lib/kernel-install.d/05-rpmostree.install`.
#[context("rpm-ostree kernel-install")]
pub fn main(argv: &[&str]) -> Result<u8> {
    let Some(layout) = std::env::var_os(LAYOUT_VAR) else {
        return Ok(0);
    };
    tracing::debug!("The LAYOUT_OSTREE is: {:?}", layout.to_str());
    if !matches!(layout.to_str(), Some(LAYOUT_OSTREE)) {
        return Ok(0);
    }
    if !ostree_ext::container_utils::is_ostree_container()? {
        eprintln!(
            "warning: confused state: {LAYOUT_VAR}={LAYOUT_OSTREE} but not in an ostree container"
        );
        return Ok(0);
    }
    let root = &Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
    tracing::debug!("argv={argv:?}");
    match argv {
        [_, _, "add", rest @ ..] => {
            add(root, rest)?;
            // In the case of adding a new kernel, we intercept everything else
            // today. In the future we can try to ensure we reuse what bits are there.
            Ok(SKIP)
        }
        [_, _, "remove", kver, ..] => {
            remove(root, kver)?;
            Ok(0)
        }
        _ => Ok(0),
    }
}
