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

use std::io::{BufRead, BufReader};
use std::process::Command;

use anyhow::{Context, Result};
use cap_std::fs::Dir;
use cap_std_ext::cap_std;
use cap_std_ext::dirext::CapStdExtDirExt;
use fn_error_context::context;

use crate::cmdutils::CommandRunExt;

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
/// Config paths per kernel-install(8), checked in priority order.
/// /etc takes precedence over /usr/lib (user/distro config over vendor defaults).
const KERNEL_INSTALL_CONF_ETC: &str = "etc/kernel/install.conf";
const KERNEL_INSTALL_CONF_ETC_D: &str = "etc/kernel/install.conf.d";
const KERNEL_INSTALL_CONF_USR: &str = "usr/lib/kernel/install.conf";
const KERNEL_INSTALL_CONF_USR_D: &str = "usr/lib/kernel/install.conf.d";

/// Parse a config file and return the layout value if found.
fn get_layout_from_file(file: std::fs::File) -> Result<Option<String>> {
    let buf = BufReader::new(file);
    for line in buf.lines() {
        let line = line?;
        let trimmed = line.trim();
        if let Some(value) = trimmed.strip_prefix("layout=") {
            return Ok(Some(value.to_string()));
        }
    }
    Ok(None)
}

/// Parse all *.conf files in a drop-in directory and return the layout value.
/// Files are processed in lexicographic order; later files override earlier ones.
fn get_layout_from_dropin_dir(rootfs: &Dir, dir_path: &str) -> Result<Option<String>> {
    let Some(dir) = rootfs.open_dir_optional(dir_path)? else {
        return Ok(None);
    };

    // Collect and sort entries lexicographically
    let mut entries: Vec<_> = dir
        .entries()?
        .filter_map(|entry| {
            let entry = entry.ok()?;
            let file_name = entry.file_name();
            let path = std::path::Path::new(&file_name);
            (path.extension() == Some(std::ffi::OsStr::new("conf"))).then_some(entry)
        })
        .collect();
    entries.sort_by_key(|e| e.file_name());

    let mut layout = None;
    for entry in entries {
        if let Some(file) = dir.open_optional(entry.file_name())? {
            if let Some(value) = get_layout_from_file(file.into_std())? {
                layout = Some(value);
            }
        }
    }
    Ok(layout)
}

/// Get the layout from a config directory level (main conf + drop-ins).
/// Per systemd drop-in semantics, drop-in files are parsed AFTER the main config
/// and can override values from it.
fn get_layout_from_config_dir(
    rootfs: &Dir,
    main_conf: &str,
    dropin_dir: &str,
) -> Result<Option<String>> {
    // Start with the main config file value (if it exists)
    let mut layout = None;
    if let Some(conf) = rootfs.open_optional(main_conf)? {
        layout = get_layout_from_file(conf.into_std())?;
    }

    // Drop-ins override the main config (parsed after, per systemd semantics)
    if let Some(dropin_layout) = get_layout_from_dropin_dir(rootfs, dropin_dir)? {
        layout = Some(dropin_layout);
    }

    Ok(layout)
}

/// Check if the kernel-install layout is configured as "ostree".
///
/// NOTE: We cannot simply rely on the KERNEL_INSTALL_LAYOUT environment variable
/// because this function is called in contexts where kernel-install is not running:
/// - At compose time (FilesystemScriptPrep, cliwrap_write_wrappers)
/// - During cliwrap interception of direct kernel-install calls
///
/// The shell hook 05-rpmostree.install can rely on the env var directly since
/// it's invoked by kernel-install itself, which parses the config and exports
/// the variable.
///
/// Per kernel-install(8) and systemd drop-in semantics:
/// - /etc/kernel/ takes precedence over /usr/lib/kernel/
/// - Within each directory, drop-in files (install.conf.d/*.conf) are parsed
///   AFTER the main config (install.conf) and can override its values
/// - Drop-in files are processed in lexicographic order; later files override earlier ones
#[context("Verifying kernel-install layout")]
pub fn is_ostree_layout(rootfs: &Dir) -> Result<bool> {
    // 1. Check /etc/kernel/ level (main conf + drop-ins merged)
    // /etc takes precedence over /usr/lib
    if let Some(layout) =
        get_layout_from_config_dir(rootfs, KERNEL_INSTALL_CONF_ETC, KERNEL_INSTALL_CONF_ETC_D)?
    {
        return Ok(layout == LAYOUT_OSTREE);
    }

    // 2. Check /usr/lib/kernel/ level (main conf + drop-ins merged)
    if let Some(layout) =
        get_layout_from_config_dir(rootfs, KERNEL_INSTALL_CONF_USR, KERNEL_INSTALL_CONF_USR_D)?
    {
        return Ok(layout == LAYOUT_OSTREE);
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
    undo_systemctl_wrap()?;
    println!("Generating initramfs");
    crate::initramfs::run_dracut(root, &kver)?;
    println!("Running depmod");
    redo_systemctl_wrap()?;
    Command::new("depmod")
        .args(["-a", kver])
        .run()
        .context("Invoking depmod")?;
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

#[context("Unwrapping systemctl")]
fn undo_systemctl_wrap() -> Result<()> {
    let bin_path = &Dir::open_ambient_dir("usr/bin", cap_std::ambient_authority())?;
    if !bin_path.exists("systemctl.rpmostreesave") {
        // Not wrapped, just return.
        return Ok(());
    }
    bin_path.rename("systemctl", &bin_path, "systemctl.backup")?;
    bin_path.rename("systemctl.rpmostreesave", &bin_path, "systemctl")?;
    Ok(())
}

#[context("Wrapping systemctl")]
fn redo_systemctl_wrap() -> Result<()> {
    let bin_path = &Dir::open_ambient_dir("usr/bin", cap_std::ambient_authority())?;
    if !bin_path.exists("systemctl.backup") {
        // We did not unwrap, just return.
        return Ok(());
    }
    bin_path.rename("systemctl", &bin_path, "systemctl.rpmostreesave")?;
    bin_path.rename("systemctl.backup", &bin_path, "systemctl")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::path::Path;

    use cap_std_ext::cap_tempfile;

    use super::*;

    #[test]
    fn test_ostree_layout_usr_conf() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;
        assert!(!is_ostree_layout(&td).unwrap());
        td.create_dir_all(Path::new(KERNEL_INSTALL_CONF_USR).parent().unwrap())?;
        td.write(KERNEL_INSTALL_CONF_USR, "")?;
        assert!(!is_ostree_layout(&td).unwrap());
        td.write(
            KERNEL_INSTALL_CONF_USR,
            indoc::indoc! { r#"
            # some comments

            layout=bacon
        "# },
        )?;
        assert!(!is_ostree_layout(&td).unwrap());
        td.write(
            KERNEL_INSTALL_CONF_USR,
            indoc::indoc! { r#"
            # this is an ostree layout
            layout=ostree
            # another comment
        "# },
        )?;

        assert!(is_ostree_layout(&td).unwrap());

        Ok(())
    }

    #[test]
    fn test_ostree_layout_usr_dropin() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;

        // No config at all
        assert!(!is_ostree_layout(&td).unwrap());

        // Create the drop-in directory
        td.create_dir_all(KERNEL_INSTALL_CONF_USR_D)?;

        // Drop-in file without layout=ostree
        td.write(
            format!("{}/00-layout.conf", KERNEL_INSTALL_CONF_USR_D),
            indoc::indoc! { r#"
            # some config
            layout=bls
        "# },
        )?;
        assert!(!is_ostree_layout(&td).unwrap());

        // Drop-in file with layout=ostree
        td.write(
            format!("{}/00-layout.conf", KERNEL_INSTALL_CONF_USR_D),
            indoc::indoc! { r#"
            # kernel-install will not try to run dracut and allow rpm-ostree to
            # take over. Rpm-ostree will use this to know that it is responsible
            # to run dracut and ensure that there is only one kernel in the image
            layout=ostree
        "# },
        )?;
        assert!(is_ostree_layout(&td).unwrap());

        Ok(())
    }

    #[test]
    fn test_ostree_layout_dropin_only() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;

        // Only drop-in, no main install.conf
        td.create_dir_all(KERNEL_INSTALL_CONF_USR_D)?;
        td.write(
            format!("{}/00-layout.conf", KERNEL_INSTALL_CONF_USR_D),
            "layout=ostree\n",
        )?;
        assert!(is_ostree_layout(&td).unwrap());

        Ok(())
    }

    #[test]
    fn test_ostree_layout_dropin_ordering() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;

        // Create the drop-in directory
        td.create_dir_all(KERNEL_INSTALL_CONF_USR_D)?;

        // First file sets ostree, second file overrides to bls
        // Later files (lexicographically) should win
        td.write(
            format!("{}/00-ostree.conf", KERNEL_INSTALL_CONF_USR_D),
            "layout=ostree\n",
        )?;
        td.write(
            format!("{}/99-bls.conf", KERNEL_INSTALL_CONF_USR_D),
            "layout=bls\n",
        )?;
        assert!(!is_ostree_layout(&td).unwrap());

        // Now reverse: bls first, ostree second - ostree should win
        td.write(
            format!("{}/00-bls.conf", KERNEL_INSTALL_CONF_USR_D),
            "layout=bls\n",
        )?;
        td.write(
            format!("{}/99-ostree.conf", KERNEL_INSTALL_CONF_USR_D),
            "layout=ostree\n",
        )?;
        assert!(is_ostree_layout(&td).unwrap());

        Ok(())
    }

    #[test]
    fn test_ostree_layout_etc_takes_precedence() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;

        // Set up /usr/lib with ostree layout
        td.create_dir_all(Path::new(KERNEL_INSTALL_CONF_USR).parent().unwrap())?;
        td.write(KERNEL_INSTALL_CONF_USR, "layout=ostree\n")?;
        assert!(is_ostree_layout(&td).unwrap());

        // Now /etc overrides to bls - should take precedence
        td.create_dir_all(Path::new(KERNEL_INSTALL_CONF_ETC).parent().unwrap())?;
        td.write(KERNEL_INSTALL_CONF_ETC, "layout=bls\n")?;
        assert!(!is_ostree_layout(&td).unwrap());

        // /etc with ostree should also work
        td.write(KERNEL_INSTALL_CONF_ETC, "layout=ostree\n")?;
        assert!(is_ostree_layout(&td).unwrap());

        Ok(())
    }

    #[test]
    fn test_ostree_layout_etc_dropin_precedence() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;

        // Set up /usr/lib/kernel/install.conf.d with ostree
        td.create_dir_all(KERNEL_INSTALL_CONF_USR_D)?;
        td.write(
            format!("{}/00-layout.conf", KERNEL_INSTALL_CONF_USR_D),
            "layout=ostree\n",
        )?;
        assert!(is_ostree_layout(&td).unwrap());

        // /etc/kernel/install.conf.d overrides - should take precedence
        td.create_dir_all(KERNEL_INSTALL_CONF_ETC_D)?;
        td.write(
            format!("{}/00-layout.conf", KERNEL_INSTALL_CONF_ETC_D),
            "layout=bls\n",
        )?;
        assert!(!is_ostree_layout(&td).unwrap());

        Ok(())
    }

    /// Test the critical scenario this PR fixes: drop-in overrides main config
    /// within the same directory level.
    ///
    /// Real-world scenario:
    /// - systemd-udev installs /usr/lib/kernel/install.conf with layout=bls
    /// - bootc adds /usr/lib/kernel/install.conf.d/00-kernel-layout.conf with layout=ostree
    /// - Expected: drop-in overrides main conf → layout=ostree
    #[test]
    fn test_ostree_layout_dropin_overrides_main_conf() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;

        // Main conf has layout=bls (simulating systemd-udev default)
        td.create_dir_all(Path::new(KERNEL_INSTALL_CONF_USR).parent().unwrap())?;
        td.write(KERNEL_INSTALL_CONF_USR, "layout=bls\n")?;
        assert!(!is_ostree_layout(&td).unwrap());

        // Drop-in overrides to layout=ostree (simulating bootc config)
        td.create_dir_all(KERNEL_INSTALL_CONF_USR_D)?;
        td.write(
            format!("{}/00-kernel-layout.conf", KERNEL_INSTALL_CONF_USR_D),
            "layout=ostree\n",
        )?;
        // Drop-in should override main conf per systemd semantics!
        assert!(is_ostree_layout(&td).unwrap());

        Ok(())
    }

    /// Test reverse scenario: main conf has ostree, drop-in overrides to bls
    #[test]
    fn test_dropin_overrides_main_conf_to_non_ostree() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;

        // Main conf has layout=ostree
        td.create_dir_all(Path::new(KERNEL_INSTALL_CONF_USR).parent().unwrap())?;
        td.write(KERNEL_INSTALL_CONF_USR, "layout=ostree\n")?;
        assert!(is_ostree_layout(&td).unwrap());

        // Drop-in overrides to layout=bls
        td.create_dir_all(KERNEL_INSTALL_CONF_USR_D)?;
        td.write(
            format!("{}/99-override.conf", KERNEL_INSTALL_CONF_USR_D),
            "layout=bls\n",
        )?;
        // Drop-in should override main conf
        assert!(!is_ostree_layout(&td).unwrap());

        Ok(())
    }

    /// Test /etc drop-in overrides /etc main conf
    #[test]
    fn test_etc_dropin_overrides_etc_main_conf() -> Result<()> {
        let td = &cap_tempfile::tempdir(cap_std::ambient_authority())?;

        // /etc main conf has layout=bls
        td.create_dir_all(Path::new(KERNEL_INSTALL_CONF_ETC).parent().unwrap())?;
        td.write(KERNEL_INSTALL_CONF_ETC, "layout=bls\n")?;
        assert!(!is_ostree_layout(&td).unwrap());

        // /etc drop-in overrides to layout=ostree
        td.create_dir_all(KERNEL_INSTALL_CONF_ETC_D)?;
        td.write(
            format!("{}/50-ostree.conf", KERNEL_INSTALL_CONF_ETC_D),
            "layout=ostree\n",
        )?;
        // Drop-in should override main conf
        assert!(is_ostree_layout(&td).unwrap());

        Ok(())
    }
}
