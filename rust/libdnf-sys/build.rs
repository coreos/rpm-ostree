// SPDX-License-Identifier: Apache-2.0 OR MIT
use anyhow::Result;

fn main() -> Result<()> {
    let libs = system_deps::Config::new().probe()?;
    let has_gpgme_pkgconfig = libs.get_by_name("gpgme").is_some();
    let with_zck: u8 = libs.get_by_name("zck").is_some().into();

    // first, the submodule proper
    let libdnf = cmake::Config::new("../../libdnf")
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
        // We don't need docs
        .define("WITH_HTML:BOOL", "0")
        .define("WITH_MAN:BOOL", "0")
        // Auto-enable zchunk, if present
        .define("WITH_ZCHUNK:BOOL", format!("{}", with_zck))
        // Don't need bindings
        .define("WITH_BINDINGS:BOOL", "0")
        // Needed in Koji at least because timestamps(?)
        // cause cmake to rerun without our -D flags which
        // breaks the build.
        .always_configure(false)
        .build_target("all")
        .build();
    // NOTE(lucab): consider using `gpgme-config` it this stops working.
    if !has_gpgme_pkgconfig {
        println!("cargo:rustc-link-lib=gpgme");
    }
    println!(
        "cargo:rustc-link-search=native={}/build/libdnf",
        libdnf.display()
    );
    println!("cargo:rustc-link-lib=static=dnf");

    // now, our thin cxx.rs bridge wrapper
    let mut libdnfcxx = cxx_build::bridge("lib.rs");
    libdnfcxx
        .file("cxx/libdnf.cxx")
        .flag("-std=c++17")
        .include("cxx") // this is needed for cxx.rs' `include!("libdnf.hxx")` to work
        .include("../../libdnf");
    // until https://github.com/gdesmott/system-deps/pull/32
    libdnfcxx.includes(libs.iter().flat_map(|lib| lib.1.include_paths.iter()));
    libdnfcxx.compile("libdnfcxx.a");

    Ok(())
}
