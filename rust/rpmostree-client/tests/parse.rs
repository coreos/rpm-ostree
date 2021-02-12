use anyhow::Result;
use rpmostree_client;

#[test]
fn parse_workstation() -> Result<()> {
    let data = include_str!("fixtures/workstation-status.json");
    let state: rpmostree_client::Status = serde_json::from_str(data)?;
    assert_eq!(state.deployments.len(), 2);
    Ok(())
}
