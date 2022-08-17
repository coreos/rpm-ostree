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
pub(crate) struct UnitConfig<'a> {
    /// If provided, will be used as the name of the unit
    pub(crate) name: Option<&'a str>,
    /// Unit/Service properties, e.g. DynamicUser=yes
    pub(crate) properties: &'a [&'a str],
    /// The command to execute
    pub(crate) exec_args: &'a [&'a str],
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
    let mut cmd = Command::new("systemd-run");
    cmd.args(BASE_ARGS);
    if let Some(name) = cfg.name {
        cmd.arg("--unit");
        cmd.arg(name);
    }
    for prop in cfg.properties.iter() {
        cmd.arg("--property");
        cmd.arg(prop);
    }
    // This ensures that this unit won't escape our process.
    cmd.arg(format!("--property=BindsTo={}", SELF_UNIT));
    cmd.arg(format!("--property=After={}", SELF_UNIT));
    cmd.arg("--");
    cmd.args(cfg.exec_args);
    let status = cmd.status()?;
    if !status.success() {
        return Err(anyhow!("{}", status));
    }
    Ok(())
}

/// Return a prepared subprocess configuration that will run as an unprivileged user if possible.
///
/// This currently only drops privileges when run under systemd with DynamicUser.
pub(crate) fn unprivileged_subprocess(binary: &str) -> Command {
    // TODO: if we detect we're running in a container as uid 0, perhaps at least switch to the
    // "bin" user if we can?
    if !crate::utils::running_in_systemd() {
        return Command::new(binary);
    }
    let mut cmd = Command::new("setpriv");
    cmd.args(&[
        "--no-new-privs",
        "--init-groups",
        "--reuid",
        "rpm-ostree",
        "--bounding-set",
        "-all",
        "--pdeathsig",
        "SIGTERM",
        "--",
        binary,
    ]);
    cmd
}

/// Return a Command instance that will re-execute the current binary.
pub(crate) fn self_command() -> Command {
    Command::new("/proc/self/exe")
}
