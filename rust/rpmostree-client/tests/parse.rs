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
    Ok(())
}
