//!
// SPDX-License-Identifier: LGPL-2.1-or-later

use crate::cxxrsutil::*;
use crate::live;
use anyhow::{anyhow, Result};
use clap::Parser;
use glib::Variant;
use ostree_ext::{gio, glib, ostree, prelude::*};
use std::process::Command;

#[derive(Debug, Parser)]
#[clap(name = "apply-live")]
#[clap(rename_all = "kebab-case")]
struct Opts {
    /// Target provided commit instead of pending deployment
    #[clap(long)]
    target: Option<String>,

    /// Reset back to booted commit
    #[clap(long)]
    reset: bool,

    /// Allow replacement of packages/files (default is pure additive)
    #[clap(long)]
    allow_replacement: bool,
}

fn get_args_variant(sysroot: &ostree::Sysroot, opts: &Opts) -> Result<glib::Variant> {
    let dict = glib::VariantDict::new(None);

    if let Some(target) = opts.target.as_ref() {
        if opts.reset {
            return Err(anyhow!("Cannot specify both --target and --reset"));
        }
        dict.insert(live::OPT_TARGET, target.as_str());
    } else if opts.reset {
        let booted = sysroot.require_booted_deployment()?;
        let csum = booted.csum();
        dict.insert(live::OPT_TARGET, csum.as_str());
    }

    if opts.allow_replacement {
        dict.insert(live::OPT_REPLACE, true);
    }

    Ok(dict.end())
}

pub(crate) fn applylive_entrypoint(args: &Vec<String>) -> CxxResult<()> {
    let opts = &Opts::parse_from(args.iter());
    let mut client = crate::client::ClientConnection::new()?;
    let sysroot = &ostree::Sysroot::new_default();
    sysroot.load(gio::Cancellable::NONE)?;

    let args_variant = get_args_variant(sysroot, opts)?;
    let params = Variant::tuple_from_iter([args_variant]);

    let reply = client
        .get_os_ex_proxy()
        .call_sync("LiveFs", Some(&params), gio::DBusCallFlags::NONE, -1, gio::Cancellable::NONE)?;

    let txn_address = reply
        .get::<(String,)>()
        .ok_or_else(|| anyhow!("Invalid reply {:?}, expected (s)", reply.type_()))?;

    client.transaction_connect_progress_sync(&txn_address.0)?;
    applylive_finish(sysroot.reborrow_cxx())?;
    Ok(())
}

/// Helper: reload systemd daemon safely
fn reload_systemd() -> Result<()> {
    let output = Command::new("/usr/bin/systemctl")
        .arg("daemon-reload")
        .status()?;
    if !output.success() {
        return Err(anyhow!("Failed to reload systemd manager configuration"));
    }
    Ok(())
}

/// Helper: diff two commits for given paths
fn compute_diff(repo: &ostree::Repo, from: &str, to: &str, path: Option<&str>) -> Result<ostree_ext::diff::Diff> {
    Ok(ostree_ext::diff::diff(repo, from, to, path)?)
}

pub(crate) fn applylive_finish(sysroot: &crate::ffi::OstreeSysroot) -> CxxResult<()> {
    let sysroot = sysroot.glib_reborrow();
    let cancellable = gio::Cancellable::NONE;
    sysroot.load_if_changed(cancellable)?;
    let repo = &sysroot.repo();
    let booted = &sysroot.require_booted_deployment()?;
    let booted_commit = booted.csum().as_str();

    let live_state = live::get_live_state(repo, booted)?
        .ok_or_else(|| anyhow!("Failed to find expected apply-live state"))?;

    let pkgdiff = {
        cxx::let_cxx_string!(from = booted_commit);
        cxx::let_cxx_string!(to = live_state.commit.as_str());
        crate::ffi::rpmdb_diff(repo.reborrow_cxx(), &from, &to, false)
            .map_err(anyhow::Error::msg)?
    };
    pkgdiff.print();

    if pkgdiff.n_removed() == 0 && pkgdiff.n_modified() == 0 {
        crate::ffi::output_message("Successfully updated running filesystem tree.");
        return Ok(());
    }

    // Compute diffs for /usr/lib/systemd/system and /usr/etc/systemd/system
    let lib_diff = compute_diff(repo, booted_commit, live_state.commit.as_str(), Some("/usr/lib/systemd/system"))?;
    let etc_diff = compute_diff(repo, booted_commit, live_state.commit.as_str(), Some("/usr/etc/systemd/system"))?;

    // Reload systemd if there are new/changed service files
    if !lib_diff.changed_files.is_empty()
        || !etc_diff.changed_files.is_empty()
        || !lib_diff.added_files.is_empty()
        || !etc_diff.added_files.is_empty()
    {
        if let Err(e) = reload_systemd() {
            crate::ffi::output_message(&format!("Warning: {}", e));
        }
    }

    let changed_services: Vec<String> = lib_diff
        .changed_files
        .union(&etc_diff.changed_files)
        .filter(|s| s.contains(".service"))
        .cloned()
        .collect();

    if !changed_services.is_empty() {
        crate::ffi::output_message(
            "Successfully updated running filesystem tree; following services may need to be restarted:",
        );
        for service in changed_services {
            crate::ffi::output_message(service.strip_prefix('/').unwrap_or(&service));
        }
    }

    Ok(())
}
