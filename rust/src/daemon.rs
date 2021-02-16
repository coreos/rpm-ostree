//! Rust portion of rpmostreed-deployment-utils.cxx

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
