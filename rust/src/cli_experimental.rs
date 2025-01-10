// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;
use camino::Utf8PathBuf;
use clap::{Parser, ValueEnum};
use std::fmt::Display;

#[derive(Debug, Parser)]
#[clap(rename_all = "kebab-case")]
/// Main options struct
struct Experimental {
    #[clap(subcommand)]
    cmd: Cmd,
}

#[derive(Debug, clap::Subcommand)]
#[clap(rename_all = "kebab-case")]
/// Subcommands
enum Cmd {
    /// Verbs for (container) build time operations.
    Build {
        #[clap(subcommand)]
        cmd: BuildCmd,
    },
}

/// Choice of build backend.
#[derive(Debug, Clone, Default, clap::ValueEnum)]
enum Mechanism {
    /// Use rpm-ostree to construct the root.
    #[default]
    RpmOstree,
    /// Use dnf to construct the root.
    Dnf,
}

impl Display for Mechanism {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.to_possible_value().unwrap().get_name().fmt(f)
    }
}

#[derive(Debug, clap::Subcommand)]
#[clap(rename_all = "kebab-case")]
/// Subcommands
enum BuildCmd {
    /// Initialize a root filesystem tree from a set of packages,
    /// including setting up mounts for the API filesystems.
    ///
    /// All configuration for rpm/dnf will come from the source root.
    InitRootFromManifest {
        /// Path to source root used for base rpm/dnf configuration.
        #[clap(long, required = true)]
        source_root: Utf8PathBuf,

        /// Path to rpm-ostree treefile.
        #[clap(long, required = true)]
        manifest: Utf8PathBuf,

        /// Path to the target root, which should not exist. However, its parent
        /// directory must exist.
        target: Utf8PathBuf,
    },
}

impl BuildCmd {
    fn run(self) -> Result<()> {
        match self {
            BuildCmd::InitRootFromManifest {
                source_root,
                manifest,
                target,
            } => {
                crate::compose::build_rootfs_from_manifest(&source_root, &manifest, &target)
            }
        }
    }
}

/// Primary entrypoint to running our wrapped `yum`/`dnf` handling.
pub fn main(argv: &[&str]) -> Result<i32> {
    let opt = Experimental::parse_from(argv.into_iter().skip(1));
    match opt.cmd {
        Cmd::Build { cmd } => cmd.run()?,
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse() -> Result<()> {
        let opt = Experimental::try_parse_from([
            "experimental",
            "build",
            "init-root",
            "--source-root=/blah",
            "/rootfs",
        ])
        .unwrap();
        match opt.cmd {
            Cmd::Build {
                cmd: BuildCmd::InitRootFromManifest { target, .. },
            } => {
                assert_eq!(target, "/rootfs");
            }
        }
        Ok(())
    }
}
