// SPDX-License-Identifier: Apache-2.0 OR MIT
use anyhow::Result;
use rpmostree_client;

#[test]
fn parse_workstation() -> Result<()> {
    let data = include_str!("fixtures/workstation-status.json");
    let state: &rpmostree_client::Status = &serde_json::from_str(data)?;
    assert_eq!(state.deployments.len(), 2);
    let booted = state.require_booted().unwrap();
    assert_eq!(booted.version.as_ref().unwrap().as_str(), "33.21");
    assert_eq!(
        booted.get_base_commit(),
        "229387d3c0bb8ad698228ca5702eca72aed8b298a7c800be1dc72bab160a9f7f"
    );
    assert!(booted.find_base_commitmeta_string("foo").is_err());
    assert_eq!(
        booted
            .find_base_commitmeta_string("coreos-assembler.config-gitrev")
            .unwrap(),
        "80966f951c766846da070b4c168b9170c61513e2"
    );
    Ok(())
}

#[test]
fn parse_booted_oci() -> Result<()> {
    let data = include_str!("fixtures/oci-deployment.json");
    let state: &rpmostree_client::Status = &serde_json::from_str(&data)?;
    assert_eq!(state.deployments.len(), 1);
    let booted = state.require_booted().unwrap();
    assert_eq!(booted.version.as_ref().unwrap().as_str(), "41.20241109.3.0");
    assert_eq!(booted.container_image_reference.as_ref().unwrap(), "ostree-unverified-registry:quay.io/fedora/fedora-coreos@sha256:d12dd2fcb57ecfde0941be604f4dcd43ce0409b86e5ee4e362184c802b80fb84");
    assert_eq!(
        booted.get_base_commit(),
        "41e8b64a8995e7412047dc0436934df69cb7886c73c2476f5743ba752dbb3e98"
    );

    // base commit meta needs to be deserialized before serde can parse it.
    let base_commit_meta = booted.get_base_manifest();

    assert!(base_commit_meta.is_ok());
    let base_commit_meta = base_commit_meta.unwrap();

    assert!(base_commit_meta.is_some());

    let base_commit_meta = base_commit_meta.unwrap();

    assert!(base_commit_meta.annotations().is_some());
    let annotations = base_commit_meta.annotations().clone().unwrap();

    let stream = annotations.get("fedora-coreos.stream");
    assert_eq!(stream.unwrap(), "stable");
    Ok(())
}
