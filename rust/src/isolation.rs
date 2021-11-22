//! APIs for multi-process isolation
// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, Result};
use fn_error_context::context;
use std::process::Command;

const SELF_UNIT: &str = "rpm-ostreed.service";
/// Run as a child process, synchronously.
const BASE_ARGS: &[&str] = &[
    "--collect",
    "--wait",
    "--pipe",
    "--no-ask-password",
    "--quiet",
];

/// Configuration for transient unit.
#[derive(Debug, Default)]
pub(crate) struct UnitConfig<'a> {
    /// If provided, will be used as the name of the unit
    pub(crate) name: Option<&'a str>,
    /// Unit/Service properties, e.g. DynamicUser=yes
    pub(crate) properties: &'a [&'a str],
    /// The command to execute
    pub(crate) exec_args: &'a [&'a str],
}

impl<'a> UnitConfig<'a> {
    /// Create a subprocess ready to execute this unit configuration via `systemd-run`.
    pub(crate) fn command(&self) -> Command {
        let mut cmd = Command::new("systemd-run");
        cmd.args(BASE_ARGS);
        if let Some(name) = self.name {
            cmd.arg("--unit");
            cmd.arg(name);
        }
        for prop in self.properties.iter() {
            cmd.arg("--property");
            cmd.arg(prop);
        }
        // This ensures that this unit won't escape our process.
        cmd.arg(format!("--property=BindsTo={}", SELF_UNIT));
        cmd.arg(format!("--property=After={}", SELF_UNIT));
        cmd.arg("--");
        cmd.args(self.exec_args);
        cmd
    }
}

/// Create a child process via `systemd-run` and synchronously wait
/// for its completion.  This runs in `--pipe` mode, so e.g. stdout/stderr
/// will go to the parent process.
/// Use this for isolation, as well as to escape the parent rpm-ostreed.service
/// isolation like `ProtectHome=true`.
#[context("Running systemd worker")]
pub(crate) fn run_systemd_worker_sync(cfg: &UnitConfig) -> Result<()> {
    if !crate::utils::running_in_systemd() {
        return Err(anyhow!("Not running under systemd"));
    }
    let mut cmd = cfg.command();
    let status = cmd.status()?;
    if !status.success() {
        return Err(anyhow!("{}", status));
    }
    Ok(())
}
