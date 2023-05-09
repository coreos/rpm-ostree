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
    let r = glib::VariantDict::new(None);

    if let Some(target) = opts.target.as_ref() {
        if opts.reset {
            return Err(anyhow!("Cannot specify both --target and --reset"));
        }
        r.insert(live::OPT_TARGET, &target.as_str());
    } else if opts.reset {
        let booted = sysroot.require_booted_deployment()?;
        // Unwrap safety: This can't return NULL
        let csum = booted.csum();
        r.insert(live::OPT_TARGET, &csum.as_str());
    }

    if opts.allow_replacement {
        r.insert(live::OPT_REPLACE, &true);
    }

    Ok(r.end())
}

pub(crate) fn applylive_entrypoint(args: &Vec<String>) -> CxxResult<()> {
    let opts = &Opts::parse_from(args.iter());
    let client = &mut crate::client::ClientConnection::new()?;
    let sysroot = &ostree::Sysroot::new_default();
    sysroot.load(gio::Cancellable::NONE)?;

    let args = get_args_variant(sysroot, opts)?;

    let params = Variant::tuple_from_iter([args]);
    let reply = &client.get_os_ex_proxy().call_sync(
        "LiveFs",
        Some(&params),
        gio::DBusCallFlags::NONE,
        -1,
        gio::Cancellable::NONE,
    )?;
    let txn_address = reply
        .get::<(String,)>()
        .ok_or_else(|| anyhow!("Invalid reply {:?}, expected (s)", reply.type_()))?;
    client.transaction_connect_progress_sync(txn_address.0.as_str())?;
    applylive_finish(sysroot.reborrow_cxx())?;
    Ok(())
}

// Postprocessing after the daemon has reported completion; print an rpmdb diff.
pub(crate) fn applylive_finish(sysroot: &crate::ffi::OstreeSysroot) -> CxxResult<()> {
    let sysroot = sysroot.glib_reborrow();
    let cancellable = gio::Cancellable::NONE;
    sysroot.load_if_changed(cancellable)?;
    let repo = &sysroot.repo();
    let booted = &sysroot.require_booted_deployment()?;
    let booted_commit = booted.csum();
    let booted_commit = booted_commit.as_str();

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
    } else {
        let lib_diff = ostree_ext::diff::diff(
            repo,
            &booted_commit,
            &live_state.commit.as_str(),
            Some("/usr/lib/systemd/system"),
        )?;

        let etc_diff = ostree_ext::diff::diff(
            repo,
            &booted_commit,
            &live_state.commit.as_str(),
            Some("/usr/etc/systemd/system"),
        )?;

        if !lib_diff.changed_files.is_empty()
            || !etc_diff.changed_files.is_empty()
            || !lib_diff.added_files.is_empty()
            || !etc_diff.added_files.is_empty()
        {
            let output = Command::new("/usr/bin/systemctl")
                .arg("daemon-reload")
                .status()
                .expect("Failed to reload systemd manager configuration");
            assert!(output.success());
        }
        let changed: Vec<String> = lib_diff
            .changed_files
            .union(&etc_diff.changed_files)
            .filter(|s| s.contains(".service"))
            .cloned()
            .collect();
        crate::ffi::output_message(
            "Successfully updated running filesystem tree; Following services may need to be restarted:");
        for service in changed {
            crate::ffi::output_message(&format!("{}", service.strip_prefix('/').unwrap()));
        }
    }
    Ok(())
}
