// SPDX-License-Identifier: Apache-2.0 OR MIT
use anyhow::Result;

fn main() -> Result<()> {
    let libs = system_deps::Config::new().probe()?;
    let has_gpgme_pkgconfig = libs.get_by_name("gpgme").is_some();
    let with_zck: u8 = libs.get_by_name("zck").is_some().into();
    let with_rhsm = std::env::var_os("CARGO_FEATURE_RHSM").is_some();

    // Query pkg-config for glib's full link flags (including transitive dependencies)
    // We'll need this later to ensure proper linking of the C++ wrapper.
    let glib_libs = pkg_config::Config::new().probe("glib-2.0")?;

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
        .define(
            "ENABLE_RHSM_SUPPORT:BOOL",
            if with_rhsm { "1" } else { "0" },
        )
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
    println!("cargo:rustc-link-lib=glib-2.0");

    // now, our thin cxx.rs bridge wrapper
    let mut libdnfcxx = cxx_build::bridge("lib.rs");
    libdnfcxx
        .file("cxx/libdnf.cxx")
        .flag("-std=c++17")
        .include("cxx") // this is needed for cxx.rs' `include!("libdnf.hpp")` to work
        .include("../../libdnf");
    libdnfcxx.includes(libs.all_include_paths());
    libdnfcxx.compile("libdnfcxx.a");

    // The C++ wrapper (libdnfcxx.a) uses both glib functions (g_strndup, g_free)
    // and libdnf functions (hy_split_nevra). Due to static library linking order,
    // we need to ensure these libraries come after libdnfcxx in the link order.
    // Even though system_deps already emitted link directives earlier, cargo's
    // link order matters - we re-emit them here to ensure symbols are available.

    // First, link libdnf again (for hy_split_nevra and other libdnf symbols)
    println!("cargo:rustc-link-lib=static=dnf");

    // Then, link glib and all its transitive dependencies (for g_strndup, g_free, etc.)
    for lib in &glib_libs.libs {
        println!("cargo:rustc-link-lib={}", lib);
    }

    Ok(())
}
