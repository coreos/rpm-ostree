// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, Result};
use clap::Parser;
use indoc::indoc;
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::Command;

use crate::ffi::SystemHostType;

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

const RPMOSTREE_INSTALL_TEXT: &str = indoc! { r#"
Install RPM packages layered on the host root filesystem.
Consider these "operating system extensions".
Add `--apply-live` to immediately start using the layered packages."#};

#[derive(Debug, Parser)]
#[clap(
    name = "yumdnf-rpmostree",
    about = "Compatibility wrapper implementing subset of yum/dnf CLI",
    version
)]
#[clap(rename_all = "kebab-case")]
/// Main options struct
struct Opt {
    /// Assume yes in answer to all questions.
    #[clap(global = true, long, short = 'y')]
    assumeyes: bool,

    #[clap(subcommand)]
    cmd: Cmd,
}

#[derive(Debug, clap::Subcommand)]
#[clap(
    name = "yumdnf-rpmostree",
    about = "Compatibility wrapper implementing subset of yum/dnf CLI",
    version
)]
#[clap(rename_all = "kebab-case")]
/// Subcommands
enum Cmd {
    /// Start an upgrade of the operating system
    Upgrade,
    /// Start an upgrade of the operating system
    Update,
    /// Display information about system state
    Status,
    /// Perform a search of packages.
    Search {
        /// Search terms
        #[allow(dead_code)]
        terms: Vec<String>,
    },
    /// Install packages.
    Install {
        /// Set of packages to install
        packages: Vec<String>,
    },
    Clean {
        subargs: Vec<String>,
    },
}

#[derive(Debug, PartialEq, Eq)]
enum RunDisposition {
    HelpOrVersionDisplayed,
    ExecRpmOstree(Vec<String>),
    UseSomethingElse,
    NotImplementedYet(&'static str),
    Unsupported,
    Unhandled,
}

fn run_clean(argv: &Vec<String>) -> Result<RunDisposition> {
    let arg = if let Some(subarg) = argv.get(0) {
        subarg
    } else {
        anyhow::bail!("Missing required argument");
    };
    match arg.as_str() {
        "all" | "metadata" | "packages" => Ok(RunDisposition::ExecRpmOstree(
            ["cleanup", "-m"].iter().map(|&s| String::from(s)).collect(),
        )),
        o => anyhow::bail!("Unknown argument: {o}"),
    }
}

fn disposition(hosttype: SystemHostType, argv: &[&str]) -> Result<RunDisposition> {
    let opt = match Opt::try_parse_from(std::iter::once(&"yum").chain(argv.iter())) {
        Ok(v) => v,
        Err(e)
            if e.kind() == clap::ErrorKind::DisplayVersion
                || e.kind() == clap::ErrorKind::DisplayHelp =>
        {
            return Ok(RunDisposition::HelpOrVersionDisplayed)
        }
        Err(_) => {
            return Ok(RunDisposition::Unhandled);
        }
    };

    let disp = match hosttype {
        SystemHostType::OstreeHost => {
            match opt.cmd {
                Cmd::Upgrade | Cmd::Update => RunDisposition::ExecRpmOstree(vec!["upgrade".into()]),
                Cmd::Status => RunDisposition::ExecRpmOstree(vec!["status".into()]),
                Cmd::Install { packages: _ } => {
                    // TODO analyze packages to find e.g. `gcc` (not ok, use `toolbox`) versus `libvirt` (ok)
                    RunDisposition::UseSomethingElse
                },
                Cmd::Clean { subargs } => {
                    run_clean(&subargs)?
                }
                Cmd::Search { .. } => RunDisposition::NotImplementedYet(indoc! { r##"
            Package search is not yet implemented.
            For now, it's recommended to use e.g. `toolbox` and `dnf search` inside there.
            "##}),
            }
        },
        SystemHostType::OstreeContainer => match opt.cmd {
            Cmd::Upgrade | Cmd::Update => RunDisposition::NotImplementedYet("At the current time, it is not supported to update packages independently of the base image."),
            Cmd::Install { mut packages } => {
                if !opt.assumeyes {
                    crate::client::warn_future_incompatibility("-y/--assumeyes is assumed now, but may not be in the future");
                }
                packages.insert(0, "install".into());
                RunDisposition::ExecRpmOstree(packages)
            },
            Cmd::Clean { subargs } => run_clean(&subargs)?,
            Cmd::Status => RunDisposition::ExecRpmOstree(vec!["status".into()]),
            Cmd::Search { .. } => {
                RunDisposition::NotImplementedYet("Package search is not yet implemented.")
            }
        },
        _ => RunDisposition::Unsupported
    };

    Ok(disp)
}

/// Primary entrypoint to running our wrapped `yum`/`dnf` handling.
pub(crate) fn main(hosttype: SystemHostType, argv: &[&str]) -> Result<()> {
    match disposition(hosttype, argv)? {
        RunDisposition::HelpOrVersionDisplayed => Ok(()),
        RunDisposition::ExecRpmOstree(args) => {
            eprintln!("{}", IMAGEBASED);
            Err(Command::new("rpm-ostree").args(args).exec().into())
        }
        RunDisposition::UseSomethingElse => {
            eprintln!("{}", IMAGEBASED);
            let mut valid_options: Vec<_> = OTHER_OPTIONS
                .iter()
                .filter(|(cmd, _)| Path::new(&format!("/usr/bin/{}", cmd)).exists())
                .collect();
            if !valid_options.is_empty() {
                eprintln!("Before installing packages to the host root filesystem, consider other options:");
            } else {
                eprintln!("To explicitly perform the operation:");
            }
            valid_options.push(&("rpm-ostree install", RPMOSTREE_INSTALL_TEXT));
            for (cmd, text) in valid_options {
                let mut lines = text.lines();
                eprintln!(" - `{}`: {}", cmd, lines.next().unwrap());
                for line in lines {
                    eprintln!("   {}", line)
                }
            }

            Err(anyhow!("not implemented"))
        }
        RunDisposition::Unhandled => Err(anyhow!("{}", UNHANDLED)),
        RunDisposition::Unsupported => Err(anyhow!(
            "This command is only supported on ostree-based systems."
        )),
        RunDisposition::NotImplementedYet(msg) => Err(anyhow!("{}\n{}", IMAGEBASED, msg)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_yumdnf() -> Result<()> {
        for common in [SystemHostType::OstreeContainer, SystemHostType::OstreeHost] {
            assert!(matches!(
                disposition(common, &["--version"])?,
                RunDisposition::HelpOrVersionDisplayed
            ));
            assert!(matches!(
                disposition(common, &["--help"])?,
                RunDisposition::HelpOrVersionDisplayed
            ));
            assert!(matches!(
                disposition(common, &["unknown", "--other"])?,
                RunDisposition::Unhandled
            ));
            assert!(matches!(
                disposition(common, &["search", "foo", "bar"])?,
                RunDisposition::NotImplementedYet(_)
            ));
        }

        // Tests for the ostree host case
        let host = SystemHostType::OstreeHost;
        assert!(matches!(
            disposition(host, &["upgrade"])?,
            RunDisposition::ExecRpmOstree(_)
        ));
        assert!(matches!(
            disposition(host, &["install", "foo"])?,
            RunDisposition::UseSomethingElse
        ));
        assert!(matches!(
            disposition(host, &["install", "foo", "bar"])?,
            RunDisposition::UseSomethingElse
        ));

        fn strvec(s: impl IntoIterator<Item = &'static str>) -> Vec<String> {
            s.into_iter().map(|s| String::from(s)).collect()
        }

        // Tests for the ostree container case
        let host = SystemHostType::OstreeContainer;
        assert_eq!(
            disposition(host, &["install", "foo", "bar"])?,
            RunDisposition::ExecRpmOstree(strvec(["install", "foo", "bar"]))
        );
        assert_eq!(
            disposition(host, &["clean", "all"])?,
            RunDisposition::ExecRpmOstree(strvec(["cleanup", "-m"]))
        );
        assert!(matches!(
            disposition(host, &["upgrade"])?,
            RunDisposition::NotImplementedYet(_)
        ));
        Ok(())
    }
}
