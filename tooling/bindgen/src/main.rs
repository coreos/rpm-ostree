// See https://bugzilla.redhat.com/show_bug.cgi?id=1608670#c3
// and https://github.com/projectatomic/rpm-ostree/pull/1516

extern crate cbindgen;

fn run() -> Result<(), String> {
    let args: Vec<String> = std::env::args().collect();
    let rustdir = std::path::Path::new(&args[1]);
    let out = std::path::Path::new("rpmostree-rust.h");

    let bindings = cbindgen::generate(rustdir).map_err(|e| e.to_string())?;
    // This uses unwraps internally; it'd be good to submit a patch
    // to add a Result-based API.
    bindings.write_to_file(out);
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
