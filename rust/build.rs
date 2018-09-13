// See https://bugzilla.redhat.com/show_bug.cgi?id=1608670#c3
// and https://github.com/projectatomic/rpm-ostree/pull/1516

extern crate cbindgen;

fn run() -> Result<(), String> {
    let out_dir_v = std::env::var("OUT_DIR").expect("OUT_DIR is unset");
    let out_dir = std::path::Path::new(&out_dir_v);

    // First, output our dependencies https://doc.rust-lang.org/cargo/reference/build-scripts.html
    println!("cargo:rerun-if-changed=cbindgen.toml");

    let bindings = cbindgen::generate(std::path::Path::new(".")).map_err(|e| e.to_string())?;
    // This uses unwraps internally; it'd be good to submit a patch
    // to add a Result-based API.
    bindings.write_to_file(out_dir.join("rpmostree-rust.h"));
    Ok(())
}

fn main() {
    match run() {
        Ok(_) => {}
        Err(e) => {
            eprintln!("error: {}", e);
            std::process::exit(1)
        }
    }
}
