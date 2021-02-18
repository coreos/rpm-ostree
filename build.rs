// SPDX-License-Identifier: Apache-2.0 OR MIT
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
    if std::env::var("CARGO_FEATURE_SANITIZERS").is_ok() {
        // Force these on
        println!("cargo:rustc-link-lib=ubsan");
        println!("cargo:rustc-link-lib=asan");
    }
    let cwd = std::env::current_dir()?;
    let cwd = cwd.to_str().expect("utf8 pwd");
    println!("cargo:rustc-link-search={}/.libs", cwd);
    println!("cargo:rustc-link-lib=static=rpmostreeinternals");
    println!(
        "cargo:rerun-if-changed={}/.libs/librpmostreeinternals.a",
        cwd
    );
    println!("cargo:rustc-link-lib=cap");
    println!("cargo:rustc-link-lib=rt");
    println!("cargo:rustc-link-lib=stdc++");
    // https://github.com/ostreedev/ostree/commit/1f832597fc83fda6cb8daf48c4495a9e1590774c
    // https://github.com/rust-lang/rust/issues/47714
    println!("cargo:rustc-link-lib=dl");
    println!("cargo:rustc-link-lib=m");
    system_deps::Config::new().probe()?;
    detect_fedora_feature()?;
    Ok(())
}
