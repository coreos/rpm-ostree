//! Helper logic for handling deployments.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use anyhow::{anyhow, format_err, Result};
use ostree_ext::glib::translate::*;
use ostree_ext::{glib, ostree};
use std::pin::Pin;

/// Get a currently unique (for this host) identifier for the deployment.
// TODO - adding the deployment timestamp would make it
// persistently unique, needs API in libostree.
pub fn deployment_generate_id(deployment: &crate::FFIOstreeDeployment) -> String {
    let deployment = deployment.glib_reborrow();
    deployment_generate_id_impl(&deployment)
}

pub(crate) fn deployment_generate_id_impl(deployment: &ostree::Deployment) -> String {
    // SAFETY: the results of these are not-nullable in the C API.
    format!(
        "{}-{}.{}",
        deployment.osname(),
        deployment.csum(),
        deployment.deployserial()
    )
}

pub fn deployment_for_id(
    ffi_sysroot: Pin<&mut crate::ffi::OstreeSysroot>,
    deploy_id: &str,
) -> CxxResult<*mut crate::FFIOstreeDeployment> {
    let sysroot = &ffi_sysroot.gobj_wrap();

    let deployment = deployment_for_id_impl(sysroot, deploy_id)?;

    let depl_ptr: *mut ostree::ffi::OstreeDeployment = deployment.to_glib_full();
    Ok(depl_ptr as *mut _)
}

fn deployment_for_id_impl(
    sysroot: &ostree::Sysroot,
    deploy_id: &str,
) -> Result<ostree::Deployment> {
    if deploy_id.is_empty() {
        return Err(anyhow!("empty deployment ID"));
    }

    for depl_entry in sysroot.deployments() {
        let id = deployment_generate_id_impl(&depl_entry);
        if deploy_id == id {
            return Ok(depl_entry);
        }
    }

    Err(anyhow!("Deployment with id '{}' not found", deploy_id))
}

pub fn deployment_checksum_for_id(
    ffi_sysroot: Pin<&mut crate::ffi::OstreeSysroot>,
    deploy_id: &str,
) -> CxxResult<String> {
    let sysroot = &ffi_sysroot.gobj_wrap();

    let deployment = deployment_for_id_impl(sysroot, deploy_id)?;
    // SAFETY: result is not-nullable in the C API.
    let csum = deployment.csum();
    Ok(csum.to_string())
}

pub fn deployment_get_base(
    ffi_sysroot: Pin<&mut crate::ffi::OstreeSysroot>,
    opt_deploy_id: &str,
    opt_os_name: &str,
) -> CxxResult<*mut crate::FFIOstreeDeployment> {
    let sysroot = &ffi_sysroot.gobj_wrap();
    let deploy_id = opt_string(opt_deploy_id);
    let os_name = opt_string(opt_os_name);

    let deployment = deployment_get_base_impl(sysroot, deploy_id, os_name)?;

    let depl_ptr: *mut ostree::ffi::OstreeDeployment = deployment.to_glib_full();
    Ok(depl_ptr as *mut _)
}

fn deployment_get_base_impl(
    sysroot: &ostree::Sysroot,
    deploy_id: Option<&str>,
    os_name: Option<&str>,
) -> Result<ostree::Deployment> {
    match deploy_id {
        Some(id) => deployment_for_id_impl(sysroot, id),
        None => sysroot.merge_deployment(os_name).ok_or_else(|| {
            let name = os_name.unwrap_or_default();
            format_err!("No deployments found for os '{}'", name)
        }),
    }
}

// Insert the pending manifest diff, if any.  Returns true iff the layers changed.
pub fn deployment_add_manifest_diff(
    dict: &crate::ffi::GVariantDict,
    diff: &crate::ffi::ExportedManifestDiff,
) -> bool {
    if diff.n_removed == 0 && diff.n_added == 0 {
        return false;
    }
    let dict = &dict.glib_reborrow();
    // Add a child dict
    let diffv = glib::VariantDict::new(None);
    diffv.insert("total", diff.total);
    diffv.insert("total-size", diff.total_size);
    diffv.insert("n-removed", diff.n_removed);
    diffv.insert("removed-size", diff.removed_size);
    diffv.insert("n-added", diff.n_added);
    diffv.insert("added-size", diff.added_size);
    dict.insert("manifest-diff", diffv);
    return true;
}
