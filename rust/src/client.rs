//! Helpers for the client side binary that will speak DBus
//! to rpm-ostreed.service.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::utils;
use anyhow::{anyhow, Result};
use gio::prelude::*;
use ostree_ext::{gio, glib};
use std::os::unix::io::IntoRawFd;
use std::pin::Pin;
use std::process::Command;

/// The well-known bus name.
const BUS_NAME: &str = "org.projectatomic.rpmostree1";
/// The global sysroot path
const SYSROOT_PATH: &str = "/org/projectatomic/rpmostree1/Sysroot";
const OS_INTERFACE: &str = "org.projectatomic.rpmostree1.OS";
const OS_EX_INTERFACE: &str = "org.projectatomic.rpmostree1.OSExperimental";

/// A unique DBus connection to the rpm-ostree daemon.
/// This currently wraps a C++ client connection.
pub(crate) struct ClientConnection {
    conn: cxx::UniquePtr<crate::ffi::ClientConnection>,
    #[allow(dead_code)]
    sysroot_proxy: gio::DBusProxy,
    #[allow(dead_code)]
    booted_proxy: gio::DBusProxy,
    booted_ex_proxy: gio::DBusProxy,
}

impl ClientConnection {
    /// Create a new connection object.
    pub(crate) fn new() -> Result<Self> {
        let mut conn = crate::ffi::new_client_connection()?;
        let mut bus_conn = conn.pin_mut().get_connection();
        let bus_conn = bus_conn.gobj_wrap();
        // This proxy will synchronously load the properties, including Booted that
        // we depend on for the next proxy.
        let sysroot_proxy = gio::DBusProxy::new_sync(
            &bus_conn,
            gio::DBusProxyFlags::NONE,
            None,
            Some(BUS_NAME),
            SYSROOT_PATH,
            "org.projectatomic.rpmostree1.Sysroot",
            gio::NONE_CANCELLABLE,
        )?;
        // Today the daemon mode requires running inside a booted deployment.
        let booted = sysroot_proxy
            .cached_property("Booted")
            .ok_or_else(|| anyhow!("Failed to find booted property"))?;
        let booted = booted
            .str()
            .ok_or_else(|| anyhow!("Booted sysroot is not a string"))?;
        let booted_proxy = gio::DBusProxy::new_sync(
            &bus_conn,
            gio::DBusProxyFlags::NONE,
            None,
            Some(BUS_NAME),
            booted,
            OS_INTERFACE,
            gio::NONE_CANCELLABLE,
        )?;
        let booted_ex_proxy = gio::DBusProxy::new_sync(
            &bus_conn,
            gio::DBusProxyFlags::NONE,
            None,
            Some(BUS_NAME),
            booted,
            OS_EX_INTERFACE,
            gio::NONE_CANCELLABLE,
        )?;
        Ok(Self {
            conn,
            sysroot_proxy,
            booted_proxy,
            booted_ex_proxy,
        })
    }

    /// Returns a proxy for the sysroot
    #[allow(dead_code)]
    pub(crate) fn get_sysroot_proxy(&self) -> &gio::DBusProxy {
        &self.sysroot_proxy
    }

    /// Returns a proxy for the booted stateroot (os)
    #[allow(dead_code)]
    pub(crate) fn get_os_proxy(&self) -> &gio::DBusProxy {
        &self.booted_proxy
    }

    /// Returns a proxy for the experimental interface to the booted stateroot (os)
    pub(crate) fn get_os_ex_proxy(&self) -> &gio::DBusProxy {
        &self.booted_ex_proxy
    }

    /// Connect to a transaction (which is a DBus socket) and monitor its progress,
    /// printing the results to stdout.  A signal handler is installed for Ctrl-C/SIGINT
    /// which will cancel the transaction.
    pub(crate) fn transaction_connect_progress_sync(&mut self, address: &str) -> Result<()> {
        Ok(self
            .conn
            .pin_mut()
            .transaction_connect_progress_sync(address)?)
    }
}

pub(crate) fn is_http_arg(arg: &str) -> bool {
    arg.starts_with("https://") || arg.starts_with("http://")
}

pub(crate) fn is_rpm_arg(arg: &str) -> bool {
    arg.ends_with(".rpm") || arg.starts_with("file://")
}

/// Given a string from the command line, determine if it represents one or more
/// RPM URLs we need to fetch, and if so download those URLs and return file
/// descriptors for the content.
/// TODO(cxx-rs): This would be slightly more elegant as Result<Option<Vec<i32>>>
pub(crate) fn client_handle_fd_argument(arg: &str, arch: &str) -> CxxResult<Vec<i32>> {
    #[cfg(feature = "fedora-integration")]
    if let Some(fds) = crate::fedora_integration::handle_cli_arg(arg, arch)? {
        return Ok(fds.into_iter().map(|f| f.into_raw_fd()).collect());
    }

    if is_http_arg(arg) {
        Ok(utils::download_url_to_tmpfile(arg, true).map(|f| vec![f.into_raw_fd()])?)
    } else if is_rpm_arg(arg) {
        match arg.strip_prefix("file://") {
            Some(rest) => Ok(vec![std::fs::File::open(rest)?.into_raw_fd()]),
            None => Ok(vec![std::fs::File::open(arg)?.into_raw_fd()]),
        }
    } else {
        Ok(Vec::new())
    }
}

/// Explicitly ensure the daemon is started via systemd, if possible.
///
/// This works around bugs from DBus activation, see
/// https://github.com/coreos/rpm-ostree/pull/2932
///
/// Basically we load too much data before claiming the bus name,
/// and dbus doesn't give us a useful error.  Instead, let's talk
/// to systemd directly and use its client tools to scrape errors.
pub(crate) fn client_start_daemon() -> CxxResult<()> {
    let service = "rpm-ostreed.service";
    // Assume non-root can't use systemd right now.
    if !nix::unistd::getuid().is_root() {
        return Ok(());
    }
    let res = Command::new("systemctl")
        .args(&["--no-ask-password", "start", service])
        .status()?;
    if !res.success() {
        let _ = Command::new("systemctl")
            .args(&["--no-pager", "status", service])
            .status();
        return Err(anyhow!("{}", res).into());
    }
    Ok(())
}

/// Convert the GVariant parameters from the DownloadProgress DBus API to a human-readable English string.
pub(crate) fn client_render_download_progress(
    mut progress: Pin<&mut crate::ffi::GVariant>,
) -> String {
    let progress = progress.gobj_wrap();
    let progress = progress
        .get::<(
            (u64, u64),
            (u32, u32),
            (u32, u32, u32),
            (u32, u32, u32, u64),
            (u32, u32),
            (u64, u64),
        )>()
        .unwrap();
    let (
        (_start_time, _elapsed_secs),
        (outstanding_fetches, outstanding_writes),
        (n_scanned_metadata, metadata_fetched, outstanding_metadata_fetches),
        (total_delta_parts, fetched_delta_parts, _total_delta_superblocks, total_delta_part_size),
        (fetched, requested),
        (bytes_transferred, bytes_sec),
    ) = progress;
    if outstanding_fetches > 0 {
        let bytes_transferred_str =
            glib::format_size_full(bytes_transferred, glib::FormatSizeFlags::empty());
        let bytes_sec_str = glib::format_size(bytes_sec);
        let bytes_sec_str = if bytes_sec == 0 {
            "-"
        } else {
            bytes_sec_str.as_str()
        };

        if total_delta_parts > 0 {
            let total_str = glib::format_size(total_delta_part_size);
            format!(
                "Receiving delta parts: {}/{} {}/s {}/{}",
                fetched_delta_parts,
                total_delta_parts,
                bytes_sec_str,
                bytes_transferred_str,
                total_str
            )
        } else if outstanding_metadata_fetches > 0 {
            format!(
                "Receiving metadata objects: {}/(estimating) {}/s {}",
                metadata_fetched, bytes_sec_str, bytes_transferred_str
            )
        } else {
            let percent = (((fetched as f64) / requested as f64) * 100f64) as u32;
            format!(
                "Receiving objects; {}% ({}/{}) {}/s {}",
                percent, fetched, requested, bytes_sec_str, bytes_transferred_str
            )
        }
    } else if outstanding_writes > 0 {
        format!("Writing objects: {}", outstanding_writes)
    } else {
        format!("Scanning metadata: {}", n_scanned_metadata)
    }
}
