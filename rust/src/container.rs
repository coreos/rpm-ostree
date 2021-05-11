//! CLI exposing `ostree-rs-ext container`

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::convert::TryInto;

use anyhow::{Context, Result};
use structopt::StructOpt;

#[derive(Debug, StructOpt)]
enum Opts {
    /// Import an ostree commit embedded in a remote container image
    Import {
        /// Path to the repository
        #[structopt(long)]
        repo: String,

        /// Image reference, e.g. registry:quay.io/exampleos/exampleos:latest
        imgref: String,
    },

    /// Print information about an exported ostree-container image.
    Info {
        /// Image reference, e.g. registry:quay.io/exampleos/exampleos:latest
        imgref: String,
    },

    /// Export an ostree commit to an OCI layout
    Export {
        /// Path to the repository
        #[structopt(long)]
        repo: String,

        /// The ostree ref or commit to export
        rev: String,

        /// Image reference, e.g. registry:quay.io/exampleos/exampleos:latest
        imgref: String,
    },
}

async fn container_import(repo: &str, imgref: &str) -> Result<()> {
    let repo = &ostree::Repo::open_at(libc::AT_FDCWD, repo, gio::NONE_CANCELLABLE)?;
    let imgref = imgref.try_into()?;
    let (tx_progress, rx_progress) = tokio::sync::watch::channel(Default::default());
    let target = indicatif::ProgressDrawTarget::stdout();
    let style = indicatif::ProgressStyle::default_bar();
    let pb = indicatif::ProgressBar::new_spinner();
    pb.set_draw_target(target);
    pb.set_style(style.template("{spinner} {prefix} {msg}"));
    pb.enable_steady_tick(200);
    pb.set_message("Downloading...");
    let import = ostree_ext::container::import(repo, &imgref, Some(tx_progress));
    tokio::pin!(import);
    tokio::pin!(rx_progress);
    loop {
        tokio::select! {
            _ = rx_progress.changed() => {
                let n = rx_progress.borrow().processed_bytes;
                pb.set_message(&format!("Processed: {}", indicatif::HumanBytes(n)));
            }
            import = &mut import => {
                pb.finish();
                println!("Imported: {}", import?.ostree_commit);
                return Ok(())
            }
        }
    }
}

async fn container_export(repo: &str, rev: &str, imgref: &str) -> Result<()> {
    let repo = &ostree::Repo::open_at(libc::AT_FDCWD, repo, gio::NONE_CANCELLABLE)?;
    let imgref = imgref.try_into()?;
    let pushed = ostree_ext::container::export(repo, rev, &imgref).await?;
    println!("{}", pushed);
    Ok(())
}

async fn container_info(imgref: &str) -> Result<()> {
    let imgref = imgref.try_into()?;
    let info = ostree_ext::container::fetch_manifest_info(&imgref).await?;
    println!("{} @{}", imgref, info.manifest_digest);
    Ok(())
}

/// Main entrypoint for container
pub fn entrypoint(args: &[&str]) -> Result<()> {
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("Failed to build tokio runtime")?
        .block_on(async {
            match Opts::from_iter(args.iter().skip(1)) {
                Opts::Import { repo, imgref } => {
                    container_import(repo.as_str(), imgref.as_str()).await
                }
                Opts::Info { imgref } => container_info(imgref.as_str()).await,
                Opts::Export { repo, rev, imgref } => {
                    container_export(repo.as_str(), rev.as_str(), imgref.as_str()).await
                }
            }
        })?;
    Ok(())
}
