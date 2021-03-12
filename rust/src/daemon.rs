//! Rust portion of rpmostreed-deployment-utils.cxx
//! The code here is mainly involved in converting on-disk state (i.e. ostree commits/deployments)
//! into GVariant which will be serialized via DBus.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use std::pin::Pin;

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
    if deployment.is_staged() {
        dict.insert("staged", &true);
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
