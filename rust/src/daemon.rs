//! Rust portion of rpmostreed-deployment-utils.cxx
//! The code here is mainly involved in converting on-disk state (i.e. ostree commits/deployments)
//! into GVariant which will be serialized via DBus.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::{cxxrsutil::*, variant_utils};
use anyhow::{anyhow, Context, Result};
use openat_ext::OpenatDirExt;
use std::io::prelude::*;
use std::os::unix::net::{UnixListener, UnixStream};
use std::os::unix::prelude::FromRawFd;
use std::pin::Pin;

/// Validate basic assumptions on daemon startup.
pub(crate) fn daemon_sanitycheck_environment(
    mut sysroot: Pin<&mut crate::FFIOstreeSysroot>,
) -> CxxResult<()> {
    let sysroot = &sysroot.gobj_wrap();
    let sysroot_dir = openat::Dir::open(format!("/proc/self/fd/{}", sysroot.get_fd()))?;
    let loc = crate::composepost::TRADITIONAL_RPMDB_LOCATION;
    if let Some(metadata) = sysroot_dir.metadata_optional(loc)? {
        let t = metadata.simple_type();
        if t != openat::SimpleType::Symlink {
            return Err(
                anyhow::anyhow!("/{} must be a symbolic link, but is: {:?}", loc, t).into(),
            );
        }
    }
    Ok(())
}

/// Connect to the client socket and ensure the daemon is initialized;
/// this avoids DBus and ensures that we get any early startup errors
/// returned cleanly.
pub(crate) fn start_daemon_via_socket() -> CxxResult<()> {
    let address = "/run/rpm-ostree/client.sock";
    let s = UnixStream::connect(address)
        .with_context(|| anyhow!("Failed to connect to {}", address))?;
    let mut s = std::io::BufReader::new(s);
    let mut r = String::new();
    s.read_to_string(&mut r)
        .context("Reading from client socket")?;
    if r.is_empty() {
        Ok(())
    } else {
        Err(anyhow!("{}", r).into())
    }
}

fn send_init_result_to_client(client: &UnixStream, err: &Result<()>) {
    let mut client = std::io::BufWriter::new(client);
    match err {
        Ok(_) => {
            // On successwe close the stream without writing anything,
            // which acknowledges successful startup to the client.
        }
        Err(e) => {
            let msg = e.to_string();
            match client
                .write_all(msg.as_bytes())
                .and_then(|_| client.flush())
            {
                Ok(_) => {}
                Err(inner_err) => {
                    eprintln!(
                        "Failed to write error message to client socket (original error: {}): {}",
                        e, inner_err
                    );
                }
            }
        }
    }
}

fn process_clients(listener: UnixListener, res: &Result<()>) {
    for stream in listener.incoming() {
        match stream {
            Ok(stream) => send_init_result_to_client(&stream, res),
            Err(e) => {
                // This shouldn't be fatal, we continue to start up.
                eprintln!("Failed to listen for client stream: {}", e);
            }
        }
        if res.is_err() {
            break;
        }
    }
}

/// Perform initialization steps required by systemd service activation.
///
/// This ensures that the system is running under systemd, then receives the
/// socket-FD for main IPC logic, and notifies systemd about ready-state.
pub(crate) fn daemon_main(debug: bool) -> Result<()> {
    if !systemd::daemon::booted()? {
        return Err(anyhow!("not running as a systemd service"));
    }

    let init_res: Result<()> = crate::ffi::daemon_init_inner(debug).map_err(|e| e.into());

    let mut fds = systemd::daemon::listen_fds(false)?.iter();
    let listener = match fds.next() {
        None => {
            // If started directly via `systemctl start` or DBus activation, we
            // directly propagate the error back to our exit code.
            init_res?;
            UnixListener::bind("/run/rpmostreed.socket")?
        }
        Some(fd) => {
            if fds.next().is_some() {
                return Err(anyhow!("Expected exactly 1 fd from systemd activation"));
            }
            let listener = unsafe { UnixListener::from_raw_fd(fd) };
            match init_res {
                Ok(_) => listener,
                Err(e) => {
                    let err_copy = Err(anyhow!("{}", e));
                    process_clients(listener, &err_copy);
                    return Err(e);
                }
            }
        }
    };

    // On success, we spawn a helper thread that just responds with
    // sucess to clients that connect via the socket.  In the future,
    // perhaps we'll expose an API here.
    std::thread::spawn(move || process_clients(listener, &Ok(())));

    // And now, enter the main loop.
    Ok(crate::ffi::daemon_main_inner()?)
}

/// Get a currently unique (for this host) identifier for the
/// deployment; TODO - adding the deployment timestamp would make it
/// persistently unique, needs API in libostree.
pub(crate) fn deployment_generate_id(
    mut deployment: Pin<&mut crate::FFIOstreeDeployment>,
) -> String {
    let deployment = deployment.gobj_wrap();
    // unwrap safety: These can't actually return NULL
    format!(
        "{}-{}.{}",
        deployment.get_osname().unwrap(),
        deployment.get_csum().unwrap(),
        deployment.get_deployserial()
    )
}

/// Serialize information about the given deployment into the `dict`;
/// this will be exposed via DBus and is hence public API.
pub(crate) fn deployment_populate_variant(
    mut sysroot: Pin<&mut crate::FFIOstreeSysroot>,
    mut deployment: Pin<&mut crate::FFIOstreeDeployment>,
    mut dict: Pin<&mut crate::FFIGVariantDict>,
) -> CxxResult<()> {
    let sysroot = &sysroot.gobj_wrap();
    let deployment = &deployment.gobj_wrap();
    let dict = dict.gobj_wrap();

    let id = deployment_generate_id(deployment.gobj_rewrap());
    // First, basic values from ostree
    dict.insert("id", &id);

    dict.insert("osname", &deployment.get_osname().expect("osname").as_str());
    dict.insert("checksum", &deployment.get_csum().expect("csum").as_str());
    dict.insert_value(
        "serial",
        &glib::Variant::from(deployment.get_deployserial() as i32),
    );

    let booted: bool = sysroot
        .get_booted_deployment()
        .map(|b| b.equal(deployment))
        .unwrap_or_default();
    dict.insert("booted", &booted);

    let live_state =
        crate::live::get_live_apply_state(sysroot.gobj_rewrap(), deployment.gobj_rewrap())?;
    if !live_state.inprogress.is_empty() {
        dict.insert("live-inprogress", &live_state.inprogress.as_str());
    }
    if !live_state.commit.is_empty() {
        dict.insert("live-replaced", &live_state.commit.as_str());
    }

    /* Staging status */
    dict.insert("staged", &deployment.is_staged());
    if deployment.is_staged() {
        if std::path::Path::new("/run/ostree/staged-deployment-locked").exists() {
            dict.insert("finalization-locked", &true);
        }
    }

    dict.insert("pinned", &deployment.is_pinned());
    let unlocked = deployment.get_unlocked();
    // Unwrap safety: This always returns a value
    dict.insert(
        "unlocked",
        &ostree::Deployment::unlocked_state_to_string(unlocked)
            .unwrap()
            .as_str(),
    );

    Ok(())
}

/// Load basic layering metadata about a deployment commit.
pub(crate) fn deployment_layeredmeta_from_commit(
    mut deployment: Pin<&mut crate::FFIOstreeDeployment>,
    mut commit: Pin<&mut crate::FFIGVariant>,
) -> CxxResult<crate::ffi::DeploymentLayeredMeta> {
    let deployment = deployment.gobj_wrap();
    let commit = &commit.gobj_wrap();
    let metadata = &variant_utils::variant_tuple_get(commit, 0).expect("commit metadata");
    let dict = &glib::VariantDict::new(Some(metadata));

    // More recent versions have an explicit clientlayer attribute (which
    // realistically will always be TRUE). For older versions, we just
    // rely on the treespec being present. */
    let is_layered = variant_utils::variant_dict_lookup_bool(dict, "rpmostree.clientlayer")
        .unwrap_or_else(|| dict.contains("rpmostree.spec"));
    if !is_layered {
        Ok(crate::ffi::DeploymentLayeredMeta {
            is_layered,
            base_commit: deployment.get_csum().unwrap().into(),
            clientlayer_version: 0,
        })
    } else {
        let base_commit = ostree::commit_get_parent(commit)
            .expect("commit parent")
            .into();
        let clientlayer_version = dict
            .lookup_value("rpmostree.clientlayer_version", Some(&*variant_utils::TY_U))
            .map(|u| u.get().unwrap())
            .unwrap_or_default();
        Ok(crate::ffi::DeploymentLayeredMeta {
            is_layered,
            base_commit,
            clientlayer_version,
        })
    }
}

/// Load basic layering metadata about a deployment
pub(crate) fn deployment_layeredmeta_load(
    mut repo: Pin<&mut crate::FFIOstreeRepo>,
    mut deployment: Pin<&mut crate::FFIOstreeDeployment>,
) -> CxxResult<crate::ffi::DeploymentLayeredMeta> {
    let repo = repo.gobj_wrap();
    let deployment = deployment.gobj_wrap();
    let commit = &repo.load_variant(
        ostree::ObjectType::Commit,
        deployment.get_csum().unwrap().as_str(),
    )?;
    deployment_layeredmeta_from_commit(deployment.gobj_rewrap(), commit.gobj_rewrap())
}
