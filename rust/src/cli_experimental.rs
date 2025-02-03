// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;
use clap::Parser;

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
    /// This command does nothing, it's a placeholder for future expansion.
    #[clap(hide = true)]
    Stub,
    /// Options for building.
    Compose {
        #[clap(subcommand)]
        cmd: ComposeCmd,
    },
}

#[derive(Debug, clap::Subcommand)]
enum ComposeCmd {
    BuildChunkedOCI {
        #[clap(flatten)]
        opts: crate::compose::BuildChunkedOCIOpts,
    },
    Rootfs {
        #[clap(flatten)]
        opts: crate::compose::RootfsOpts,
    },
    CommitToContainerRootfs {
        #[clap(flatten)]
        opts: crate::compose::CommitToContainerRootfsOpts,
    },
}

impl ComposeCmd {
    fn run(self) -> Result<()> {
        match self {
            ComposeCmd::BuildChunkedOCI { opts } => opts.run(),
            ComposeCmd::Rootfs { opts } => opts.run(),
            ComposeCmd::CommitToContainerRootfs { opts } => opts.run(),
        }
    }
}

impl Cmd {
    fn run(self) -> Result<()> {
        match self {
            Cmd::Stub => {
                println!("Did nothing successfully.");
                Ok(())
            }
            Cmd::Compose { cmd } => cmd.run(),
        }
    }
}

/// Primary entrypoint to running our wrapped `yum`/`dnf` handling.
pub fn main(argv: &[&str]) -> Result<i32> {
    let opt = Experimental::parse_from(argv.into_iter().skip(1));
    opt.cmd.run()?;
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse() -> Result<()> {
        let opt = Experimental::try_parse_from(["experimental", "stub"]).unwrap();
        match opt.cmd {
            Cmd::Stub => {}
            o => {
                panic!("Unexpected {o:?}")
            }
        }
        Ok(())
    }
}
