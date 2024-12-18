// If not running on container continue the current path.
// SPDX-License-Identifier: Apache-2.0 OR MIT
use crate::cliwrap::cliutil;
use anyhow::Result;
use camino::Utf8Path;
use cap_std::fs::FileType;
use cap_std::fs_utf8::Dir as Utf8Dir;
use cap_std_ext::cap_std;
use fn_error_context::context;

/// Primary entrypoint to running our wrapped `kernel-install` handling.
#[context("rpm-ostree kernel-install wrapper")]
pub(crate) fn main(argv: &[&str]) -> Result<()> {
    if !ostree_ext::container_utils::is_ostree_container()? {
        return cliutil::exec_real_binary("kernel-install", argv);
    }
    let is_install = matches!(argv.first(), Some(&"add"));

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
        crate::initramfs::run_dracut(&k)?;
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
