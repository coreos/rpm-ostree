//!
// SPDX-License-Identifier: LGPL-2.1-or-later

use crate::cxxrsutil::*;
use crate::live;
use anyhow::{anyhow, Result};
use gio::prelude::*;
use glib::Variant;
use std::pin::Pin;
use structopt::StructOpt;

#[derive(Debug, StructOpt)]
#[structopt(name = "apply-live")]
#[structopt(rename_all = "kebab-case")]
struct Opts {
    /// Target provided commit instead of pending deployment
    #[structopt(long)]
    target: Option<String>,
    /// Reset back to booted commit
    #[structopt(long)]
    reset: bool,

    /// Allow replacement of packages/files (default is pure additive)
    #[structopt(long)]
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
        let csum = booted.csum().expect("csum");
        r.insert(live::OPT_TARGET, &csum.as_str());
    }

    if opts.allow_replacement {
        r.insert(live::OPT_REPLACE, &true);
    }

    Ok(r.end())
}

pub(crate) fn applylive_entrypoint(args: &Vec<String>) -> CxxResult<()> {
    let opts = &Opts::from_iter(args.iter());
    let client = &mut crate::client::ClientConnection::new()?;
    let sysroot = &ostree::Sysroot::new_default();
    sysroot.load(gio::NONE_CANCELLABLE)?;

    let args = get_args_variant(sysroot, opts)?;

    let params = Variant::from_tuple(&[args]);
    let reply = &client.get_os_ex_proxy().call_sync(
        "LiveFs",
        Some(&params),
        gio::DBusCallFlags::NONE,
        -1,
        gio::NONE_CANCELLABLE,
    )?;
    let txn_address = reply
        .get::<(String,)>()
        .ok_or_else(|| anyhow!("Invalid reply {:?}, expected (s)", reply.type_()))?;
    client.transaction_connect_progress_sync(txn_address.0.as_str())?;
    applylive_finish(sysroot.gobj_rewrap())?;
    Ok(())
}

// Postprocessing after the daemon has reported completion; print an rpmdb diff.
pub(crate) fn applylive_finish(mut sysroot: Pin<&mut crate::ffi::OstreeSysroot>) -> CxxResult<()> {
    let sysroot = sysroot.gobj_wrap();
    let cancellable = gio::NONE_CANCELLABLE;
    sysroot.load_if_changed(cancellable)?;
    let repo = &sysroot.repo().unwrap();
    let booted = &sysroot.require_booted_deployment()?;
    let booted_commit = booted.csum().expect("csum");
    let booted_commit = booted_commit.as_str();

    let live_state = live::get_live_state(repo, booted)?
        .ok_or_else(|| anyhow!("Failed to find expected apply-live state"))?;

    let pkgdiff = {
        cxx::let_cxx_string!(from = booted_commit);
        cxx::let_cxx_string!(to = live_state.commit.as_str());
        let repo = repo.gobj_rewrap();
        crate::ffi::rpmdb_diff(repo, &from, &to, false).map_err(anyhow::Error::msg)?
    };
    pkgdiff.print();

    if pkgdiff.n_removed() == 0 && pkgdiff.n_modified() == 0 {
        crate::ffi::output_message("Successfully updated running filesystem tree.");
    } else {
        crate::ffi::output_message(
            "Successfully updated running filesystem tree; some services may need to be restarted.",
        );
    }
    Ok(())
}
