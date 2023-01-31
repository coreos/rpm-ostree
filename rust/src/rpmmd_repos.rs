//! Core implementation logic for "apply-live" which applies
//! changes to an overlayfs on top of `/usr` in the booted
//! deployment.
/*
 * Copyright (C) 2023 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//use crate::utils;
use anyhow::Result;
use fn_error_context::context;

/// Primary entrypoint to running our wrapped `config-manager` handling.
#[context("dnf config-manager wrapper")]
pub fn entrypoint(args: &[&str]) -> Result<()> {
    if let Some(arg) = args.get(1) {
        match arg {
        &"--add-repo" => add_repo(),
        &"--set-enabled" => set_enabled(),
        o => anyhow::bail!("Unknown argument {o}")
        }
    } else {
        // TODO FIX message.
        anyhow::bail!("No argument provided")
    }
}

/// --add-repo
fn add_repo() -> Result<()> {
    //let temp_file = utils::download_url_to_tmpfile("arg", true).map(|f| vec![f.into_raw_fd()]);
    print!("test add-repo");
    Ok(())
}

/// --set-enabled
fn set_enabled() -> Result<()> {
    print!("test set_enabled");
    Ok(())
}