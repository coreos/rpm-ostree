// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, Result};
use indoc::formatdoc;
use indoc::indoc;
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::Command;
use structopt::StructOpt;

/// Emitted at the first line.
pub(crate) const IMAGEBASED: &str = "Note: This system is image (rpm-ostree) based.";

/// Emitted for unhandled options.
const UNHANDLED: &str = indoc::indoc! {r#"
rpm-ostree: Unknown arguments passed to yum/dnf compatibility wrapper.

Please see `yum --help` for currently accepted commands via this
wrapper, and `rpm-ostree --help` for more information about.the rpm-ostree
project.
"#};

const OTHER_OPTIONS: &[(&str, &str)] = &[
    (
        "toolbox",
        "For command-line development and debugging tools in a privileged container",
    ),
    ("podman", "General purpose containers"),
    ("docker", "General purpose containers"),
    ("flatpak", "Desktop (GUI) applications"),
];

#[derive(Debug, StructOpt)]
#[structopt(
    name = "yumdnf-rpmostree",
    about = "Compatibility wrapper implementing subset of yum/dnf CLI"
)]
#[structopt(rename_all = "kebab-case")]
/// Main options struct
enum Opt {
    /// Start an upgrade of the operating system
    Upgrade,
    /// Start an upgrade of the operating system
    Update,
    /// Display information about system state
    Status,
    /// Perform a search of packages.
    Search {
        /// Search terms
        terms: Vec<String>,
    },
    /// Will return an error suggesting other approaches.
    Install {
        /// Set of packages to install
        packages: Vec<String>,
    },
}

#[derive(Debug, PartialEq, Eq)]
enum RunDisposition {
    HelpOrVersionDisplayed,
    ExecRpmOstree(Vec<String>),
    UseSomethingElse,
    NotImplementedYet(&'static str),
    Unhandled,
}

fn disposition(argv: &[&str]) -> Result<RunDisposition> {
    let opt = match Opt::from_iter_safe(std::iter::once(&"yum").chain(argv.iter())) {
        Ok(v) => v,
        Err(e)
            if e.kind == clap::ErrorKind::VersionDisplayed
                || e.kind == clap::ErrorKind::HelpDisplayed =>
        {
            return Ok(RunDisposition::HelpOrVersionDisplayed)
        }
        Err(_) => {
            return Ok(RunDisposition::Unhandled);
        }
    };

    let disp = match opt {
        Opt::Upgrade | Opt::Update => RunDisposition::ExecRpmOstree(vec!["upgrade".into()]),
        Opt::Status => RunDisposition::ExecRpmOstree(vec!["status".into()]),
        Opt::Install { packages: _ } => {
            // TODO analyze packages to find e.g. `gcc` (not ok, use `toolbox`) versus `libvirt` (ok)
            RunDisposition::UseSomethingElse
        }
        Opt::Search { .. } => RunDisposition::NotImplementedYet(indoc! { r##"
            Package search is not yet implemented.
            For now, it's recommended to use e.g. `toolbox` and `dnf search` inside there.
            "##}),
    };

    Ok(disp)
}

/// Primary entrypoint to running our wrapped `yum`/`dnf` handling.
pub(crate) fn main(argv: &[&str]) -> Result<()> {
    match disposition(argv)? {
        RunDisposition::HelpOrVersionDisplayed => Ok(()),
        RunDisposition::ExecRpmOstree(args) => {
            eprintln!("{}", IMAGEBASED);
            Err(Command::new("rpm-ostree").args(args).exec().into())
        }
        RunDisposition::UseSomethingElse => {
            eprintln!("{}", IMAGEBASED);
            let mut valid_options = String::new();
            for (cmd, text) in OTHER_OPTIONS {
                if Path::new(&format!("/usr/bin/{}", cmd)).exists() {
                    valid_options.push_str(&format!("\n - `{}`: {}", cmd, text));
                }
            }
            let msg = formatdoc! {r#"{}
            Before installing packages to the host root filesystem, consider other options:{}
             - `rpm-ostree install`: Install RPM packages layered on the host root filesystem.
                Consider these "operating system extensions".
                Add `--apply-live` to immediately start using the layered packages."#, IMAGEBASED, valid_options.as_str()};
            Err(anyhow!("{}", msg))
        }
        RunDisposition::Unhandled => Err(anyhow!("{}", UNHANDLED)),
        RunDisposition::NotImplementedYet(msg) => Err(anyhow!("{}\n{}", IMAGEBASED, msg)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_yumdnf() -> Result<()> {
        assert!(matches!(
            disposition(&["--version"])?,
            RunDisposition::HelpOrVersionDisplayed
        ));
        assert!(matches!(
            disposition(&["--help"])?,
            RunDisposition::HelpOrVersionDisplayed
        ));
        assert!(matches!(
            disposition(&["unknown", "--other"])?,
            RunDisposition::Unhandled
        ));
        assert!(matches!(
            disposition(&["upgrade"])?,
            RunDisposition::ExecRpmOstree(_)
        ));
        assert!(matches!(
            disposition(&["install", "foo"])?,
            RunDisposition::UseSomethingElse
        ));
        assert!(matches!(
            disposition(&["install", "foo", "bar"])?,
            RunDisposition::UseSomethingElse
        ));
        assert!(matches!(
            disposition(&["search", "foo", "bar"])?,
            RunDisposition::NotImplementedYet(_)
        ));
        Ok(())
    }
}
