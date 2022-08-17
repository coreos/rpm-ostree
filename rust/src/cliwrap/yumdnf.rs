// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, Result};
use clap::Parser;
use indoc::indoc;
use std::os::unix::process::CommandExt;
use std::process::Command;

use crate::ffi::SystemHostType;

/// Emitted at the first line.
pub(crate) const IMAGEBASED: &str = "Note: This system is image (rpm-ostree) based.";

// const OTHER_OPTIONS: &[(&str, &str)] = &[
//     (
//         "toolbox",
//         "For command-line development and debugging tools in a privileged container",
//     ),
//     ("podman", "General purpose containers"),
//     ("docker", "General purpose containers"),
//     ("flatpak", "Desktop (GUI) applications"),
// ];
// const RPMOSTREE_INSTALL_TEXT: &str = indoc! { r#"
// Install RPM packages layered on the host root filesystem.
// Consider these "operating system extensions".
// Add `--apply-live` to immediately start using the layered packages."#};

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
    /// Operate on ostree-based bootable container images
    Image {
        #[clap(subcommand)]
        cmd: ImageCmd,
    },
}

/// Switch the booted container image.
#[derive(Debug, Parser)]
#[clap(rename_all = "kebab-case")]
struct RebaseCmd {
    /// Explicitly opt-out of requiring any form of signature verification.
    #[clap(long)]
    no_signature_verification: bool,

    /// Opt-in notice this is still experimental
    #[clap(long)]
    experimental: bool,

    /// Use this ostree remote for signature verification
    #[clap(long)]
    ostree_remote: Option<String>,

    /// The transport; e.g. oci, oci-archive.  Defaults to `registry`.
    #[clap(long, default_value = "registry")]
    transport: String,

    /// Target container image reference
    imgref: String,
}

/// Operations on container images
#[derive(Debug, clap::Subcommand)]
#[clap(rename_all = "kebab-case")]
enum ImageCmd {
    Rebase(RebaseCmd),
    Build {
        #[clap(subcommand)]
        cmd: BuildCmd,
    },
}

/// Commands to build container images
#[derive(Debug, clap::Subcommand)]
#[clap(rename_all = "kebab-case")]
enum BuildCmd {
    #[clap(external_subcommand)]
    ImageFromTreeFile(Vec<String>),
}

#[derive(Debug, PartialEq, Eq)]
enum RunDisposition {
    ExecRpmOstree(Vec<String>),
    NotImplementedYet(&'static str),
    OnlySupportedOn(SystemHostType),
    Unsupported,
}

impl RebaseCmd {
    fn to_ostree_container_ref(&self) -> Result<ostree_ext::container::OstreeImageReference> {
        let transport = ostree_ext::container::Transport::try_from(self.transport.as_str())?;
        let imgref = ostree_ext::container::ImageReference {
            transport,
            name: self.imgref.to_string(),
        };
        use ostree_ext::container::SignatureSource;
        let sigverify = if self.no_signature_verification {
            SignatureSource::ContainerPolicyAllowInsecure
        } else if let Some(remote) = self.ostree_remote.as_ref() {
            SignatureSource::OstreeRemote(remote.to_string())
        } else {
            SignatureSource::ContainerPolicy
        };
        Ok(ostree_ext::container::OstreeImageReference { sigverify, imgref })
    }
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

fn disposition(opt: Opt, hosttype: SystemHostType) -> Result<RunDisposition> {
    let disp = match hosttype {
        SystemHostType::OstreeHost => {
            match opt.cmd {
                Cmd::Upgrade | Cmd::Update => RunDisposition::ExecRpmOstree(vec!["upgrade".into()]),
                Cmd::Status => RunDisposition::ExecRpmOstree(vec!["status".into()]),
                Cmd::Install { packages } => {
                    let mut args = packages;
                    args.insert(0, "install".into());
                    args.insert(1, "-A".into());
                    if opt.assumeyes {
                        args.insert(1, "-y".into());
                    }
                    RunDisposition::ExecRpmOstree(args)
                },
                Cmd::Clean { subargs } => {
                    run_clean(&subargs)?
                }
                Cmd::Search { .. } => RunDisposition::NotImplementedYet(indoc! { r##"
            Package search is not yet implemented.
            For now, it's recommended to use e.g. `toolbox` and `dnf search` inside there.
            "##}),
                Cmd::Image { cmd } => {
                    match cmd {
                        ImageCmd::Rebase(rebase) => {
                            let container_ref = rebase.to_ostree_container_ref()?;
                            let container_ref = container_ref.to_string();
                            let experimental = rebase.experimental.then(|| "--experimental");
                            let cmd = ["rebase"].into_iter().chain(experimental).chain([container_ref.as_str()])
                                .map(|s| s.to_string()).collect::<Vec<String>>();
                            RunDisposition::ExecRpmOstree(cmd)
                        },
                        ImageCmd::Build { cmd } => {
                            match cmd {
                                BuildCmd::ImageFromTreeFile(mut args) => {
                                    args.insert(0, "compose".to_string());
                                    args.insert(1, "image".to_string());
                                    RunDisposition::ExecRpmOstree(args)
                                }
                            }
                        }
                    }
            }
            }
        },
        SystemHostType::OstreeContainer => match opt.cmd {
            Cmd::Upgrade | Cmd::Update => RunDisposition::NotImplementedYet("At the current time, it is not supported to update packages independently of the base image."),
            Cmd::Install { packages } => {
                let mut args = packages;
                if !opt.assumeyes {
                    crate::client::warn_future_incompatibility("-y/--assumeyes is assumed now, but may not be in the future");
                }
                args.insert(0, "install".into());
                if opt.assumeyes {
                    args.insert(1, "-y".into());
                }
                RunDisposition::ExecRpmOstree(args)
            },
            Cmd::Clean { subargs } => run_clean(&subargs)?,
            Cmd::Status => RunDisposition::ExecRpmOstree(vec!["status".into()]),
            Cmd::Search { .. } => {
                RunDisposition::NotImplementedYet("Package search is not yet implemented.")
            },
            Cmd::Image { .. } => {
                RunDisposition::OnlySupportedOn(SystemHostType::OstreeHost)
            },
        },
        _ => RunDisposition::Unsupported
    };

    Ok(disp)
}

/// Primary entrypoint to running our wrapped `yum`/`dnf` handling.
pub(crate) fn main(hosttype: SystemHostType, argv: &[&str]) -> Result<()> {
    let opt = Opt::parse_from(std::iter::once(&"yum").chain(argv.iter()));
    match disposition(opt, hosttype)? {
        RunDisposition::ExecRpmOstree(args) => {
            eprintln!("{}", IMAGEBASED);
            Err(Command::new("rpm-ostree").args(args).exec().into())
        }
        // TODO *maybe* enable this later via something like a personality flag
        // that discourages package installs on e.g. CoreOS.
        // RunDisposition::UseSomethingElse => {
        //     eprintln!("{}", IMAGEBASED);
        //     let mut valid_options: Vec<_> = OTHER_OPTIONS
        //         .iter()
        //         .filter(|(cmd, _)| Path::new(&format!("/usr/bin/{}", cmd)).exists())
        //         .collect();
        //     if !valid_options.is_empty() {
        //         eprintln!("Before installing packages to the host root filesystem, consider other options:");
        //     } else {
        //         eprintln!("To explicitly perform the operation:");
        //     }
        //     valid_options.push(&("rpm-ostree install", RPMOSTREE_INSTALL_TEXT));
        //     for (cmd, text) in valid_options {
        //         let mut lines = text.lines();
        //         eprintln!(" - `{}`: {}", cmd, lines.next().unwrap());
        //         for line in lines {
        //             eprintln!("   {}", line)
        //         }
        //     }

        //     Err(anyhow!("not implemented"))
        // }
        RunDisposition::Unsupported => Err(anyhow!(
            "This command is only supported on ostree-based systems."
        )),
        RunDisposition::OnlySupportedOn(platform) => {
            let platform = crate::client::system_host_type_str(&platform);
            Err(anyhow!("This command is only supported on {platform}"))
        }
        RunDisposition::NotImplementedYet(msg) => Err(anyhow!("{}\n{}", IMAGEBASED, msg)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn testrun(hosttype: SystemHostType, args: &[&str]) -> Result<RunDisposition> {
        let o = Opt::try_parse_from([&"yum"].into_iter().chain(args)).unwrap();
        disposition(o, hosttype)
    }

    #[test]
    fn test_yumdnf() -> Result<()> {
        for common in [SystemHostType::OstreeContainer, SystemHostType::OstreeHost] {
            assert!(matches!(
                testrun(common, &["search", "foo", "bar"])?,
                RunDisposition::NotImplementedYet(_)
            ));
        }

        let rebasecmd = &[
            "image",
            "rebase",
            "--experimental",
            "quay.io/example/os:latest",
        ];

        fn vecstr(v: impl IntoIterator<Item = &'static str>) -> Vec<String> {
            v.into_iter().map(|s| s.to_string()).collect()
        }

        // Tests for the ostree host case
        let host = SystemHostType::OstreeHost;
        assert!(matches!(
            testrun(host, &["upgrade"])?,
            RunDisposition::ExecRpmOstree(_)
        ));
        assert_eq!(
            testrun(host, &["install", "foo"])?,
            RunDisposition::ExecRpmOstree(vecstr(["install", "-A", "foo"]))
        );
        assert_eq!(
            testrun(host, &["install", "-y", "foo", "bar"])?,
            RunDisposition::ExecRpmOstree(vecstr(["install", "-y", "-A", "foo", "bar"]))
        );
        assert!(matches!(
            testrun(host, rebasecmd).unwrap(),
            RunDisposition::ExecRpmOstree(_)
        ));

        fn strvec(s: impl IntoIterator<Item = &'static str>) -> Vec<String> {
            s.into_iter().map(String::from).collect()
        }

        // Tests for the ostree container case
        let host = SystemHostType::OstreeContainer;
        assert_eq!(
            testrun(host, &["install", "foo", "bar"])?,
            RunDisposition::ExecRpmOstree(strvec(["install", "foo", "bar"]))
        );
        assert_eq!(
            testrun(host, &["clean", "all"])?,
            RunDisposition::ExecRpmOstree(strvec(["cleanup", "-m"]))
        );
        assert!(matches!(
            testrun(host, rebasecmd).unwrap(),
            RunDisposition::OnlySupportedOn(SystemHostType::OstreeHost)
        ));

        assert!(matches!(
            testrun(host, &["upgrade"])?,
            RunDisposition::NotImplementedYet(_)
        ));
        Ok(())
    }
}
