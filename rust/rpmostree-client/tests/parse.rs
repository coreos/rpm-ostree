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
    Ok(())
}
