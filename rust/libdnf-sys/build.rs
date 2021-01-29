use anyhow::Result;

fn main() -> Result<()> {
    system_deps::Config::new().probe()?;
    use cmake::Config;
    let libdnf = Config::new("../../libdnf")
        // Needed for hardened builds
        .cxxflag("-fPIC")
        // I picked /usr/libexec/rpm-ostree just because we need an
        // arbitrary path - we don't actually install there.
        .define("CMAKE_INSTALL_PREFIX:PATH", "/usr/libexec/rpm-ostree")
        .define(
            "INCLUDE_INSTALL_DIR:PATH",
            "/usr/libexec/rpm-ostree/include",
        )
        .define("LIB_INSTALL_DIR:PATH", "/usr/libexec/rpm-ostree")
        .define("SYSCONF_INSTALL_DIR:PATH", "/usr/libexec/rpm-ostree/etc")
        .define("SHARE_INSTALL_PREFIX:PATH", "/usr/libexec/rpm-ostree/share")
        .define("ENABLE_STATIC:BOOL", "1")
        .define("CMAKE_POSITION_INDEPENDENT_CODE", "ON")
        // rpm-ostree maintains its own state
        .define("WITH_SWDB:BOOL", "0")
        // We don't need docs
        .define("WITH_HTML:BOOL", "0")
        .define("WITH_MAN:BOOL", "0")
        // Don't need bindings
        .define("WITH_BINDINGS:BOOL", "0")
        .define("WITH_GIR:BOOL", "0")
        .build_target("all")
        .build();
    println!(
        "cargo:rustc-link-search=native={}/build/libdnf",
        libdnf.display()
    );
    println!("cargo:rustc-link-lib=static=dnf");
    Ok(())
}
