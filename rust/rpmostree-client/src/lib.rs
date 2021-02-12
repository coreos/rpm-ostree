//! APIs for interacting with rpm-ostree client side.

use anyhow::Context;
use serde_derive::Deserialize;
use std::process::Command;

/// Our generic catchall fatal error, expected to be converted
/// to a string to output to a terminal or logs.
type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync + 'static>>;

/// Representation of the rpm-ostree client-side state; this
/// can be parsed directly from the output of `rpm-ostree status --json`.
/// Currently not all fields are here, but that is a bug.
#[derive(Deserialize)]
#[serde(rename_all = "kebab-case")]
pub struct Status {
    pub deployments: Vec<Deployment>,
}

/// A single deployment, i.e. a bootable ostree commit
#[derive(Deserialize)]
#[serde(rename_all = "kebab-case")]
pub struct Deployment {
    pub unlocked: Option<String>,
    pub osname: String,
    pub pinned: bool,
    pub checksum: String,
    pub staged: Option<bool>,
    pub booted: bool,
    pub serial: u32,
    pub origin: String,
}

/// Gather a snapshot of the system status.
pub fn query_status() -> Result<Status> {
    // Retry on temporary activation failures, see
    // https://github.com/coreos/rpm-ostree/issues/2531
    let pause = std::time::Duration::from_secs(1);
    let max_retries = 10;
    let mut retries = 0;
    let cmd_res = loop {
        retries += 1;
        let res = Command::new("rpm-ostree")
            .args(&["status", "--json"])
            .output()
            .context("failed to spawn 'rpm-ostree status'")?;

        if res.status.success() || retries >= max_retries {
            break res;
        }
        std::thread::sleep(pause);
    };

    if !cmd_res.status.success() {
        return Err(format!(
            "running 'rpm-ostree status' failed: {}",
            String::from_utf8_lossy(&cmd_res.stderr)
        )
        .into());
    }

    Ok(serde_json::from_slice(&cmd_res.stdout)
        .context("failed to parse 'rpm-ostree status' output")?)
}
