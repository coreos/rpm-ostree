use anyhow::Result;

fn main() -> Result<()> {
    system_deps::Config::new().probe()?;
    Ok(())
}
