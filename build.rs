use anyhow::Result;

fn detect_fedora_feature() -> Result<()> {
    if !std::path::Path::new("/usr/lib/os-release").exists() {
        return Ok(());
    }
    let p = std::process::Command::new("sh")
        .args(&["-c", ". /usr/lib/os-release && echo ${ID}"])
        .stdout(std::process::Stdio::piped())
        .output()?;
    let out = std::str::from_utf8(&p.stdout).ok().map(|s| s.trim());
    if out == Some("fedora") {
        println!(r#"cargo:rustc-cfg=feature="fedora-integration""#)
    }
    Ok(())
}

fn main() -> Result<()> {
    detect_fedora_feature()?;
    Ok(())
}
