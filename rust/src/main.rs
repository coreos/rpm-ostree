//! The main CLI logic.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{Context, Result};
use nix::sys::signal;
use std::io::Write;
use std::os::unix::prelude::CommandExt;
use termcolor::WriteColor;

/// Directly exec(ostree admin unlock) - does not return on success.
fn usroverlay(args: &[&str]) -> Result<()> {
    // Handle --help and error on extra arguments
    let _ = clap::App::new("rpm-ostree usroverlay")
        .bin_name("rpm-ostree usroverlay")
        .long_version("")
        .long_about("Apply a transient overlayfs to /usr")
        .get_matches_from(args.iter().skip(1));
    Err::<_, anyhow::Error>(
        std::process::Command::new("ostree")
            .args(&["admin", "unlock"])
            .exec()
            .into(),
    )
    .context("Failed to execute ostree admin unlock")
}

// And now we've done process global initialization, we have a tokio runtime setup; process the command line.
// As of today, basically every function here is blocking, so we spawn a thread.
// But the idea is that in the future, we could add async-native code here too.
//
// It is only recently that our main() function is in Rust, calling
// into C++ as a library.  As of right now, the only Rust commands
// are hidden, i.e. should not appear in --help.  So we just recognize
// those, and if there's something we don't know about, invoke the C++
// main().
async fn inner_async_main(args: Vec<String>) -> Result<i32> {
    let args_borrowed: Vec<_> = args.iter().map(|s| s.as_str()).collect();
    let arg = args_borrowed.get(1).map(|s| *s).unwrap_or(&"");
    // Async-native code goes here
    match arg {
        "ex-container" => {
            return rpmostree_rust::container::entrypoint(&args_borrowed).await;
        }
        _ => {}
    }
    // Everything below here is a blocking API, and run on a worker thread so
    // that the main thread is dedicated to the Tokio reactor.
    tokio::task::spawn_blocking(move || {
        let args_borrowed: Vec<_> = args.iter().map(|s| s.as_str()).collect();
        let args = &args_borrowed[..];
        if let Some(arg) = args.get(1) {
            match *arg {
                // Add custom Rust commands here, and also in `libmain.cxx` if user-visible.
                "countme" => rpmostree_rust::countme::entrypoint(args).map(|_| 0),
                "cliwrap" => rpmostree_rust::cliwrap::entrypoint(args).map(|_| 0),
                // The `unlock` is a hidden alias for "ostree CLI compatibility"
                "usroverlay" | "unlock" => usroverlay(args).map(|_| 0),
                // C++ main
                _ => Ok(rpmostree_rust::ffi::rpmostree_main(args)?),
            }
        } else {
            Ok(rpmostree_rust::ffi::rpmostree_main(args)?)
        }
    })
    .await?
}

/// The real main function returns a `Result<>`.
fn inner_main() -> Result<i32> {
    if std::env::var("RPMOSTREE_GDB_HOOK").is_ok() {
        println!("RPMOSTREE_GDB_HOOK detected; stopping...");
        println!("Attach via gdb using `gdb -p {}`.", std::process::id());
        signal::raise(signal::Signal::SIGSTOP).expect("signal(SIGSTOP)");
    }
    // Initialize failpoints
    let _scenario = fail::FailScenario::setup();
    fail::fail_point!("main");
    // Call this early on; it invokes e.g. setenv() so must be done
    // before we create threads.
    rpmostree_rust::ffi::early_main();
    // We need to write to stderr, because some of our commands write to stdout
    // like `rpm-ostree compose tree --print-json`.
    tracing_subscriber::fmt::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::from_default_env())
        .with_writer(std::io::stderr)
        .init();
    tracing::trace!("starting");
    // Gather our arguments.
    let args: Result<Vec<String>> = std::env::args_os()
        .map(|s| -> Result<String> {
            s.into_string()
                .map_err(|s| anyhow::anyhow!("Argument is invalid UTF-8: {}", s.to_string_lossy()))
        })
        .collect();
    let args = args?;
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("Failed to build tokio runtime")?
        .block_on(inner_async_main(args))
}

fn print_error(e: anyhow::Error) {
    // See discussion in CxxResult for why we use this format
    let msg = format!("{:#}", e);
    // Print the error: prefix in red if we're on a tty
    let stderr = termcolor::BufferWriter::stderr(termcolor::ColorChoice::Auto);
    let stderrbuf = {
        let mut stderrbuf = stderr.buffer();
        let _ =
            stderrbuf.set_color(termcolor::ColorSpec::new().set_fg(Some(termcolor::Color::Red)));
        let _ = write!(&mut stderrbuf, "error: ");
        let _ = stderrbuf.reset();
        let _ = writeln!(&mut stderrbuf, "{}", msg);
        stderrbuf
    };
    let _ = stderr.print(&stderrbuf);
}

fn main() {
    // NOTE!  Don't add new code here.  Only add new code into `inner_main()`.
    // Capture any error.  Note that in some cases the C++ code may still call exit(<code>) directly.
    let r = inner_main();
    rpmostree_rust::ffi::rpmostree_process_global_teardown();
    // Print the error
    match r {
        Ok(e) => std::process::exit(e),
        Err(e) => {
            print_error(e);
            std::process::exit(1)
        }
    }
}
