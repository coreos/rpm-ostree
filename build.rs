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
    let cwd = std::env::current_dir()?;
    let cwd = cwd.to_str().expect("utf8 pwd");
    println!("cargo:rustc-link-search={}/.libs", cwd);
    println!("cargo:rustc-link-lib=static=rpmostreeinternals");
    println!("cargo:rustc-link-lib=cap");
    println!("cargo:rustc-link-search={}/libdnf-build/libdnf", cwd);
    println!("cargo:rustc-link-lib=dnf");
    println!("cargo:rustc-link-lib=rpmostree-1");
    system_deps::Config::new().probe()?;
    detect_fedora_feature()?;
    Ok(())
}
