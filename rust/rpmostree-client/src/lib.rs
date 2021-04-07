//! APIs for interacting with rpm-ostree client side.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Context;
use serde_derive::Deserialize;
use std::collections::HashMap;
use std::process::Command;

/// Our generic catchall fatal error, expected to be converted
/// to a string to output to a terminal or logs.
type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync + 'static>>;

/// Used for methods that invoke the `/usr/bin/rpm-ostree` binary directly.
/// This acts as a carrier for the `RPMOSTREE_AGENT_ID` variable which
/// is used to textually identify the caller.
#[derive(Debug, Clone)]
pub struct CliClient {
    agent_id: String,
}

impl CliClient {
    /// Create a CliClient structure which just holds an agent identifier.
    /// Choose an agent identifer that e.g. matches a systemd unit name
    /// or a binary name, not a full textual English string for example.
    ///
    /// For example, `zincati` or `mco`, not `Machine Config Operator`.
    pub fn new<S: AsRef<str>>(agent_id: S) -> Self {
        Self {
            agent_id: agent_id.as_ref().to_string(),
        }
    }
}

/// Representation of the rpm-ostree client-side state; this
/// can be parsed directly from the output of `rpm-ostree status --json`.
/// Currently not all fields are here, but that is a bug.
#[derive(Debug, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub struct Status {
    pub deployments: Vec<Deployment>,
}

/// A single deployment, i.e. a bootable ostree commit
#[derive(Debug, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub struct Deployment {
    pub unlocked: Option<String>,
    pub osname: String,
    pub pinned: bool,
    pub checksum: String,
    pub base_checksum: Option<String>,
    pub base_commit_meta: HashMap<String, serde_json::Value>,
    pub staged: Option<bool>,
    pub booted: bool,
    pub serial: u32,
    pub origin: String,
    pub version: Option<String>,
}

impl Status {
    /// Find the booted deployment, if any.
    pub fn find_booted(&self) -> Option<&Deployment> {
        self.deployments.iter().find(|d| d.booted)
    }

    /// Find the booted deployment.
    pub fn require_booted(&self) -> Result<&Deployment> {
        self.find_booted()
            .ok_or_else(|| format!("No booted deployment").into())
    }
}

impl Deployment {
    /// Find the base OSTree commit
    pub fn get_base_commit(&self) -> &str {
        self.base_checksum
            .as_deref()
            .unwrap_or(self.checksum.as_str())
    }

    /// Find a given metadata key in the base commit, which must hold a non-empty string.
    pub fn find_base_commitmeta_string<'a>(&'a self, k: &str) -> Result<&'a str> {
        let v = self.base_commit_meta.get(k);
        if let Some(v) = v {
            match v {
                serde_json::Value::String(v) => {
                    if v.is_empty() {
                        Err(format!("Invalid empty {} metadata key", k).into())
                    } else {
                        Ok(v)
                    }
                }
                _ => Err(format!("Invalid non-string {} metadata key", k).into()),
            }
        } else {
            Err(format!("No {} metadata key", k).into())
        }
    }
}

impl CliClient {
    /// Create an invocation of the client binary
    fn cli_cmd(&self) -> Command {
        let mut cmd = Command::new("rpm-ostree");
        cmd.env("RPMOSTREE_CLIENT_ID", self.agent_id.as_str());
        cmd
    }

    /// Gather a snapshot of the system status.
    pub fn query_status(&self) -> Result<Status> {
        // Retry on temporary activation failures, see
        // https://github.com/coreos/rpm-ostree/issues/2531
        let pause = std::time::Duration::from_secs(1);
        let max_retries = 10;
        let mut retries = 0;
        let cmd_res = loop {
            retries += 1;
            let res = self
                .cli_cmd()
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
}
