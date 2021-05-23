//! Helpers for the client side binary that will speak DBus
//! to rpm-ostreed.service.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::core::OSTREE_BOOTED;
use crate::cxxrsutil::*;
use crate::ffi::SystemHostType;
use crate::utils;
use anyhow::{anyhow, Context, Result};
use camino::Utf8Path;
use fn_error_context::context;
use gio::prelude::*;
use ostree_ext::{gio, glib};
use std::io::{BufRead, Write};
use std::os::unix::io::IntoRawFd;
use std::os::unix::net::UnixStream;
use std::process::Command;

/// The well-known bus name.
const BUS_NAME: &str = "org.projectatomic.rpmostree1";
/// The global sysroot path
const SYSROOT_PATH: &str = "/org/projectatomic/rpmostree1/Sysroot";
const OS_INTERFACE: &str = "org.projectatomic.rpmostree1.OS";
const OS_EX_INTERFACE: &str = "org.projectatomic.rpmostree1.OSExperimental";
const CLIENT_SOCKET_PATH: &str = "/run/rpm-ostree/client.sock";

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
        require_system_host_type(SystemHostType::OstreeHost)?;
        let mut conn = crate::ffi::new_client_connection()?;
        let bus_conn = conn.pin_mut().get_connection();
        let bus_conn = bus_conn.glib_reborrow();
        // This proxy will synchronously load the properties, including Booted that
        // we depend on for the next proxy.
        let sysroot_proxy = gio::DBusProxy::new_sync(
            &bus_conn,
            gio::DBusProxyFlags::NONE,
            None,
            Some(BUS_NAME),
            SYSROOT_PATH,
            "org.projectatomic.rpmostree1.Sysroot",
            gio::Cancellable::NONE,
        )
        .context("Initializing sysroot proxy")?;
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
            gio::Cancellable::NONE,
        )?;
        let booted_ex_proxy = gio::DBusProxy::new_sync(
            &bus_conn,
            gio::DBusProxyFlags::NONE,
            None,
            Some(BUS_NAME),
            booted,
            OS_EX_INTERFACE,
            gio::Cancellable::NONE,
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

pub(crate) fn is_src_rpm_arg(arg: &str) -> bool {
    arg.ends_with(".src.rpm")
}

/// Given a string from the command line, determine if it represents one or more
/// RPM URLs we need to fetch, and if so download those URLs and return file
/// descriptors for the content.
/// TODO(cxx-rs): This would be slightly more elegant as Result<Option<Vec<i32>>>
#[context("Handling argument {}", arg)]
pub(crate) fn client_handle_fd_argument(
    arg: &str,
    arch: &str,
    is_replace: bool,
) -> CxxResult<Vec<i32>> {
    #[cfg(feature = "fedora-integration")]
    if let Some(fds) = crate::fedora_integration::handle_cli_arg(arg, arch, is_replace)? {
        return Ok(fds.into_iter().map(|f| f.into_raw_fd()).collect());
    }

    if is_src_rpm_arg(arg) {
        return Err(anyhow!(
            "{} appears to be a source RPM which are not usually intended to be installed",
            arg
        )
        .into());
    } else if is_http_arg(arg) {
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

// If the client socket doesn't exist, try to ask systemd to start it.
// This can happen even when the socket unit is installed because
// presets may not enable it.
fn check_and_start_daemon_socket() -> Result<bool> {
    if Utf8Path::new(CLIENT_SOCKET_PATH).exists() {
        return Ok(true);
    }
    let socket_unit = "rpm-ostreed.socket";
    tracing::debug!("{CLIENT_SOCKET_PATH} does not exist, explicitly starting socket unit");
    let start_result = Command::new("systemctl")
        .args(&["--no-ask-password", "start", socket_unit])
        .output()?;
    if !start_result.status.success() {
        let err = String::from_utf8_lossy(&start_result.stderr);
        tracing::warn!("Failed to start {socket_unit}: {err}");
        return Ok(false);
    }
    Ok(true)
}

/// Connect to the client socket and ensure the daemon is initialized;
/// this avoids DBus and ensures that we get any early startup errors
/// returned cleanly.
#[context("Starting daemon via socket")]
fn start_daemon_via_socket() -> Result<()> {
    let capable = check_and_start_daemon_socket()?;
    if !capable {
        tracing::debug!("Falling back to DBus activation");
        return Ok(());
    }

    tracing::debug!("Starting daemon via {CLIENT_SOCKET_PATH}");
    let mut socket = UnixStream::connect(CLIENT_SOCKET_PATH)?;
    crate::daemon::write_message(
        &mut socket,
        crate::daemon::SocketMessage::ClientHello {
            selfid: crate::core::self_id()?,
        },
    )
    .context("Writing ClientHello")?;
    let resp = crate::daemon::recv_message(&mut socket)?;
    match resp {
        crate::daemon::SocketMessage::ServerOk => Ok(()),
        crate::daemon::SocketMessage::ServerError { msg } => {
            Err(anyhow!("server error: {msg}").into())
        }
        o => Err(anyhow!("unexpected message: {o:?}").into()),
    }
}

pub(crate) fn client_start_daemon() -> CxxResult<()> {
    // systemctl and socket paths only work for root right now; in the future
    // the socket may be opened up.
    if rustix::process::getuid().as_raw() != 0 {
        return Ok(());
    }
    return start_daemon_via_socket().map_err(Into::into);
}

/// Convert the GVariant parameters from the DownloadProgress DBus API to a human-readable English string.
pub(crate) fn client_render_download_progress(progress: &crate::ffi::GVariant) -> String {
    let progress = progress
        .glib_reborrow()
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
        format!("Writing objects: {outstanding_writes}")
    } else {
        format!("Scanning metadata: {n_scanned_metadata}")
    }
}

pub(crate) fn running_in_container() -> bool {
    ostree_ext::container_utils::running_in_container()
}

pub(crate) fn is_bare_split_xattrs() -> CxxResult<bool> {
    Ok(ostree_ext::container_utils::is_bare_split_xattrs()?)
}

pub(crate) fn is_ostree_container() -> CxxResult<bool> {
    Ok(ostree_ext::container_utils::is_ostree_container()?)
}

pub(crate) fn get_system_host_type() -> CxxResult<SystemHostType> {
    let r = if ostree_ext::container_utils::is_ostree_container()? {
        SystemHostType::OstreeContainer
    } else if std::path::Path::new(OSTREE_BOOTED).exists() {
        SystemHostType::OstreeHost
    } else {
        SystemHostType::Unknown
    };
    Ok(r)
}

pub(crate) fn system_host_type_str(t: &SystemHostType) -> &'static str {
    match *t {
        SystemHostType::OstreeContainer => "ostree container",
        SystemHostType::OstreeHost => "ostree host",
        _ => "unknown",
    }
}

/// Return an error if the current system host type does not match expected.
pub(crate) fn require_system_host_type(expected: SystemHostType) -> CxxResult<()> {
    let current = get_system_host_type()?;
    if current != expected {
        let expected = system_host_type_str(&expected);
        let current = system_host_type_str(&current);
        return Err(format!("This command requires an {expected} system; found: {current}").into());
    }
    Ok(())
}

/// Emit a warning about a potential future incompatible change we may make
/// that would require adjustment from the user.
pub fn warn_future_incompatibility(msg: impl AsRef<str>) {
    let msg = msg.as_ref();
    eprintln!("warning: {msg}");
    std::thread::sleep(std::time::Duration::from_secs(1));
}

fn is_yes(s: &str) -> bool {
    matches!(s.to_lowercase().as_str(), "y" | "yes")
}

/// Prompt for confirmation
pub(crate) fn confirm() -> CxxResult<bool> {
    let mut stdout = std::io::stdout().lock();
    let mut stdin = std::io::stdin().lock();
    write!(stdout, "Continue? [y/N] ")?;
    stdout.flush()?;
    let mut resp = String::new();
    stdin.read_line(&mut resp)?;
    Ok(is_yes(resp.as_str().trim()))
}

/// Prompt for confirmation, and return an error if not agreed
pub(crate) fn confirm_or_abort() -> CxxResult<()> {
    if confirm()? {
        Ok(())
    } else {
        Err(anyhow::format_err!("Operation aborted").into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::env;

    #[test]
    fn test_is_src_rpm() {
        let rpm = "https://fedora.org/rpms/src/kernel-2.6.1.rpm";
        let src_rpm = "file://linux-kernel-2.2.2.src.rpm";
        assert!(!is_src_rpm_arg(rpm));
        assert!(is_src_rpm_arg(src_rpm));
    }

    #[test]
    fn test_yes() {
        for v in ["y", "yes"] {
            assert!(is_yes(v))
        }
        for v in ["", "n", "no", "DOIT"] {
            assert!(!is_yes(v));
        }
    }

    #[test]
    fn test_running_in_container() {
        assert_eq!(env::var("container").is_ok(), running_in_container());
    }
}
