// If not running on container continue the current path.
// SPDX-License-Identifier: Apache-2.0 OR MIT
use crate::cliwrap;
use crate::cliwrap::cliutil;
use anyhow::{anyhow, Context, Result};
use camino::Utf8Path;
use cap_std::fs::FileType;
use cap_std::fs_utf8::Dir as Utf8Dir;
use cap_std_ext::cap_std;
use fn_error_context::context;
use std::io::Write;
use std::process::Command;

/// Primary entrypoint to running our wrapped `kernel-install` handling.
#[context("rpm-ostree kernel-install wrapper")]
pub(crate) fn main(argv: &[&str]) -> Result<()> {
    if !ostree_ext::container_utils::is_ostree_container()? {
        return cliutil::exec_real_binary("kernel-install", argv);
    }
    let is_install = matches!(argv.get(0), Some(&"add"));

    let modules_path = Utf8Dir::open_ambient_dir("lib/modules", cap_std::ambient_authority())?;
    //kernel-install is called by kernel-core and kernel-modules cleanup let's make sure we just call dracut once.
    let mut new_kernel: Option<_> = None;

    //finds which the kernel to remove and which to generate the initramfs for.
    for entry in modules_path.entries()? {
        let entry = entry?;
        let kernel_dir = entry.file_name()?;
        let kernel_binary_path = Utf8Path::new(&kernel_dir).join("vmlinuz");

        if entry.file_type()? == FileType::dir() {
            if modules_path.exists(kernel_binary_path) {
                if is_install {
                    new_kernel = Some(kernel_dir);
                }
            } else {
                new_kernel = None;
                modules_path.remove_dir_all(kernel_dir)?;
            }
        }
    }
    if let Some(k) = new_kernel {
        undo_systemctl_wrap()?;
        run_dracut(&k)?;
        redo_systemctl_wrap()?;
    }
    Ok(())
}

#[context("Unwrapping systemctl")]
fn undo_systemctl_wrap() -> Result<()> {
    let bin_path = Utf8Dir::open_ambient_dir("usr/bin", cap_std::ambient_authority())?;
    bin_path.rename("systemctl", &bin_path, "systemctl.backup")?;
    bin_path.rename("systemctl.rpmostreesave", &bin_path, "systemctl")?;
    Ok(())
}

#[context("Wrapping systemctl")]
fn redo_systemctl_wrap() -> Result<()> {
    let bin_path = Utf8Dir::open_ambient_dir("usr/bin", cap_std::ambient_authority())?;
    bin_path.rename("systemctl", &bin_path, "systemctl.rpmostreesave")?;
    bin_path.rename("systemctl.backup", &bin_path, "systemctl")?;
    Ok(())
}

#[context("Running dracut")]
fn run_dracut(kernel_dir: &str) -> Result<()> {
    let root_fs = Utf8Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
    let tmp_dir = tempfile::tempdir()?;
    let tmp_initramfs_path = tmp_dir.path().join("initramfs.img");

    let cliwrap_dracut = Utf8Path::new(cliwrap::CLIWRAP_DESTDIR).join("dracut");
    let dracut_path = cliwrap_dracut
        .exists()
        .then(|| cliwrap_dracut)
        .unwrap_or_else(|| Utf8Path::new("dracut").to_owned());
    // If changing this, also look at changing rpmostree-kernel.cxx
    let res = Command::new(dracut_path)
        .args(&[
            "--no-hostonly",
            "--kver",
            kernel_dir,
            "--reproducible",
            "-v",
            "--add",
            "ostree",
            "-f",
        ])
        .arg(&tmp_initramfs_path)
        .status()?;
    if !res.success() {
        return Err(anyhow!(
            "Failed to generate initramfs (via dracut) for for kernel: {kernel_dir}: {:?}",
            res
        ));
    }
    let mut f = std::fs::OpenOptions::new()
        .write(true)
        .append(true)
        .open(&tmp_initramfs_path)?;
    // Also append the dev/urandom bits here, see the duplicate bits in rpmostree-kernel.cxx
    f.write(crate::initramfs::get_dracut_random_cpio())?;
    drop(f);
    let utf8_tmp_dir_path = Utf8Path::from_path(tmp_dir.path().strip_prefix("/")?)
        .context("Error turning Path to Utf8Path")?;
    root_fs.rename(
        utf8_tmp_dir_path.join("initramfs.img"),
        &root_fs,
        (Utf8Path::new("lib/modules").join(kernel_dir)).join("initramfs.img"),
    )?;
    Ok(())
}
