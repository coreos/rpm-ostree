//! Handling for the `rpm-ostree exp` verb, which will be the successor to `ex`.

use anyhow::Result;
use clap::{Parser, Subcommand};

#[derive(Debug, Subcommand)]
enum ContainerCommand {
    Diff {
        /// The source/original image
        from: String,

        /// The target/destination image
        to: String,
    },
}

#[derive(Debug, Subcommand)]
enum Command {
    Container {
        #[clap(subcommand)]
        command: ContainerCommand,
    },
}

#[derive(Debug, Parser)]
/// Experimental subcommands; there are no stability guarantees.
///
/// Typically these are useful for debugging/introspection.
struct Exp {
    #[clap(subcommand)]
    command: Command,
}

impl ContainerCommand {
    async fn run(self) -> Result<()> {
        match self {
            ContainerCommand::Diff { from, to } => {
                let proxy = containers_image_proxy::ImageProxy::new().await?;
                let prev_manifest = {
                    let oi = &proxy.open_image(&from).await?;
                    proxy.fetch_manifest(oi).await?.1
                };
                let new_manifest = {
                    let oi = &proxy.open_image(&to).await?;
                    proxy.fetch_manifest(oi).await?.1
                };

                let diff = ostree_ext::container::manifest_diff(&prev_manifest, &new_manifest);
                diff.print();
                Ok(())
            }
        }
    }
}

impl Command {
    async fn run(self) -> Result<()> {
        match self {
            Command::Container { command } => command.run().await,
        }
    }
}

pub async fn entrypoint(args: Vec<String>) -> Result<()> {
    let args = Exp::parse_from(args.into_iter().skip(1));
    args.command.run().await
}
