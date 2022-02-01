//! Implementation of the "DNF Count Me" system.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{bail, Context, Result};
use cap_std_ext::rustix;
use curl::easy::Easy;
use os_release::OsRelease;
use std::path;

use crate::core::OSTREE_BOOTED;

mod cookie;
mod repo;

/// Default variant name used in User Agent
const DEFAULT_VARIANT_ID: &str = "unknown";

/// Send a request to 'url' with 'ua' as User Agent.
/// This sends a GET request and discards the body as this is what is currently
/// expected on the Fedora infrastructure side.
/// Once this is fixed, we can switch to a HEAD request to reduce the footprint:
/// let mut handle = Easy::new().nobody(true)?;
fn send_countme(url: &str, ua: &str) -> Result<()> {
    println!("Sending request to: {}", url);
    let mut handle = Easy::new();
    handle.follow_location(true)?;
    handle.fail_on_error(true)?;
    handle.url(url)?;
    handle.useragent(ua)?;
    {
        let mut transfer = handle.transfer();
        transfer.write_function(|new_data| Ok(new_data.len()))?;
        transfer.perform()?;
    }
    Ok(())
}

/// Main entrypoint for countme
pub fn entrypoint(_args: &[&str]) -> Result<()> {
    // Skip if we are not run on an ostree booted system
    if !path::Path::new(OSTREE_BOOTED).exists() {
        bail!("Not running on an ostree based system");
    }

    // Skip if we are not running with an unprivileged user
    if rustix::process::geteuid().as_raw() == 0 {
        bail!("Must run under an unprivileged user");
    }

    // Load repo configs and keep only those enabled, with a metalink and countme=1
    let repos: Vec<_> = self::repo::all()?
        .into_iter()
        .filter(|r| r.count_me())
        .collect();
    if repos.is_empty() {
        println!("No enabled repositories with countme=1");
        return Ok(());
    }

    // Load timestamp cookie
    let cookie = cookie::Cookie::new().context("Could not read existing cookie")?;

    // Skip this run if we are not in a new counting window
    if cookie.existing_window() {
        println!("Skipping: Not in a new counting window");
        return Ok(());
    }

    // Read /etc/os-release
    let release: OsRelease = OsRelease::new()?;
    let variant: &str = release
        .extra
        .get("VARIANT_ID")
        .map_or(DEFAULT_VARIANT_ID, |s| s);

    // Setup User Agent. The format is:
    // libdnf (NAME VERSION_ID; VARIANT_ID; OS.BASEARCH)
    // libdnf (Fedora 31; server; Linux.x86_64)
    // See `user_agent` option in:
    // https://dnf.readthedocs.io/en/latest/conf_ref.html?highlight=user_agent#options-for-both-main-and-repo
    let ua = format!(
        "rpm-ostree ({} {}; {}; {}.{})",
        release.name,
        release.version_id,
        variant,
        "Linux",
        std::env::consts::ARCH
    );
    println!("Using User Agent: {}", ua);

    // Compute the value to send as window counter
    let counter = cookie.get_window_counter();

    // Send Get requests, track successfully ones and do not exit on failures
    let successful = repos.iter().fold(0, |acc, r| {
        let url = format!("{}&countme={}", &r.metalink(&release.version_id), counter);
        match send_countme(&url, &ua) {
            Ok(_) => acc + 1,
            Err(e) => {
                eprintln!("Request '{}' failed: {}", url, e);
                acc
            }
        }
    });

    // Update cookie timestamp only if at least one request is successful
    if successful == 0 {
        bail!("No request successful");
    }
    println!("Successful requests: {}/{}", successful, repos.len());
    if let Err(e) = cookie.persist() {
        // Do not exit with a non zero code here as we have still made at least
        // one successful request thus we have been counted.
        eprintln!("Failed to persist cookie: {}", e);
    }
    Ok(())
}
