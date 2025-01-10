//! The main CLI logic.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{Context, Result};
use is_terminal::IsTerminal;
use rpmostree_rust::builtins;
use std::ffi::OsString;
use std::io::Write;
use termcolor::WriteColor;

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
    // Everything below here is a blocking API, and run on a worker thread so
    // that the main thread is dedicated to the Tokio reactor.
    tokio::task::spawn_blocking(move || -> Result<i32, anyhow::Error> {
        let args_orig = args;
        let args_borrowed: Vec<_> = args_orig.iter().map(|s| s.as_str()).collect();
        let args = &args_borrowed[..];
        if let Some(arg) = args.get(1) {
            match *arg {
                // Add custom Rust commands here, and also in `libmain.cxx` if user-visible.
                "countme" => rpmostree_rust::countme::entrypoint(args).map(|_| 0),
                "kernel-install" => rpmostree_rust::kernel_install::main(args).map(Into::into),
                "fix-shadow-perms" => rpmostree_rust::passwd::fix_shadow_perms_entrypoint(args).map(|_| 0),
                "cliwrap" => rpmostree_rust::cliwrap::entrypoint(args).map(|_| 0),
                // A hidden wrapper to intercept some binaries in RPM scriptlets.
                "scriptlet-intercept" => builtins::scriptlet_intercept::entrypoint(args).map(|_| 0),
                // This is a deprecated entrypoint
                "container-encapsulate" => {
                    rpmostree_rust::client::warn_future_incompatibility(
                    "This entrypoint is deprecated; use `rpm-ostree compose container-encapsulate` instead",
                    );
                    rpmostree_rust::container::container_encapsulate(args_orig).map(|_| 0)
                    .map_err(anyhow::Error::msg)
                },
                "experimental" => {
                    rpmostree_rust::cli_experimental::main(args)
                }
                // C++ main
                _ => Ok(rpmostree_rust::ffi::rpmostree_main(args)?),
            }
        } else {
            Ok(rpmostree_rust::ffi::rpmostree_main(args)?)
        }
    })
    .await?
}

/// Invoke the ostree-ext CLI code.
async fn dispatch_ostree_ext(args: Vec<String>) -> Result<i32> {
    ostree_ext::cli::run_from_iter(args).await?;
    Ok(0)
}

/// Dispatch multicall binary to relevant logic, based on callname from `argv[0]`.
async fn dispatch_multicall(callname: String, args: Vec<String>) -> Result<i32> {
    match callname.as_str() {
        "ostree-container" | "ostree-ima-sign" | "ostree-provisional-repair" => {
            dispatch_ostree_ext(args).await
        }
        _ => inner_async_main(args).await, // implicitly includes "rpm-ostree"
    }
}

/// Process a string from `argv[0]` into a clean callname.
fn callname_from_argv0(argv0: &str) -> &str {
    let callname = argv0.rsplit_once('/').map(|t| t.1).unwrap_or(argv0);
    if callname.is_empty() {
        "rpm-ostree"
    } else {
        callname
    }
}

/// Gather arguments from command-line, and clean up `argv[0]` for multicall.
fn gather_cli_args(argv: impl IntoIterator<Item = OsString>) -> Result<(String, Vec<String>)> {
    let args: Result<Vec<String>> = argv
        .into_iter()
        .map(|s| {
            s.into_string()
                .map_err(|s| anyhow::anyhow!("Argument is invalid UTF-8: {}", s.to_string_lossy()))
        })
        .collect();
    let mut args = args?;

    // NOTE: `args` is guaranteed non-empty from here on.
    if args.is_empty() {
        args.push("rpm-ostree".to_string());
    }

    if args[0].is_empty() {
        args[0] = "rpm-ostree".to_string();
    }

    let callname = callname_from_argv0(&args[0]).to_string();

    Ok((callname, args))
}

/// The real main function returns a `Result<>`.
fn inner_main() -> Result<i32> {
    if std::env::var("RPMOSTREE_GDB_HOOK").is_ok() {
        println!("RPMOSTREE_GDB_HOOK detected; stopping...");
        println!("Attach via gdb using `gdb -p {}`.", std::process::id());
        rustix::process::kill_current_process_group(rustix::process::Signal::Stop)
            .expect("signal(SIGSTOP)");
    }
    // Initialize failpoints
    let _scenario = fail::FailScenario::setup();
    rpmostree_rust::try_fail_point!("main");
    // Call this early on; it invokes e.g. setenv() so must be done
    // before we create threads.
    rpmostree_rust::ffi::early_main();
    // Logging goes to stderr, because stdout is used for output by some of
    // our commands like `rpm-ostree compose tree --print-json`.
    // Don't include timestamps and such because they're not really useful and
    // too verbose, and plus several log targets such as journald will already
    // include timestamps.  Relatedly, if we're running in systemd, don't include colors
    // because that will make our messages appear as binary data in the journal.
    let format = tracing_subscriber::fmt::format()
        .without_time()
        .with_target(false)
        .with_ansi(!rpmostree_rust::utils::running_in_systemd())
        .compact();
    tracing_subscriber::fmt::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::from_default_env())
        .event_format(format)
        .with_writer(std::io::stderr)
        .init();
    tracing::trace!("starting");

    // Gather and pre-process command-line arguments.
    let (callname, args) = gather_cli_args(std::env::args_os())?;

    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .context("Failed to build tokio runtime")?;
    runtime.block_on(dispatch_multicall(callname, args))
}

fn print_error(e: anyhow::Error) {
    // See discussion in CxxResult for why we use this format
    let msg = format!("{:#}", e);
    // Print the error: prefix in red if we're on a tty
    let colored = if std::io::stderr().is_terminal() {
        termcolor::ColorChoice::Auto
    } else {
        termcolor::ColorChoice::Never
    };
    let stderr = termcolor::BufferWriter::stderr(colored);
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_callname_from_argv0() {
        let testcases = [
            ("foo", "foo"),
            ("/usr/bin/foo", "foo"),
            ("", "rpm-ostree"),
            ("/usr/bin/", "rpm-ostree"),
        ];

        for (input, expected) in testcases {
            let output = callname_from_argv0(input);
            assert_eq!(output, expected);
        }
    }

    #[test]
    fn test_gather_cli_args() {
        let testcases = [
            (vec![], ("rpm-ostree", vec!["rpm-ostree"])),
            (vec![""], ("rpm-ostree", vec!["rpm-ostree"])),
            (vec!["foo"], ("foo", vec!["foo"])),
            (
                vec!["/usr/bin/foo", "bar"],
                ("foo", vec!["/usr/bin/foo", "bar"]),
            ),
            (
                vec!["/usr/bin/", "bar"],
                ("rpm-ostree", vec!["/usr/bin/", "bar"]),
            ),
        ];
        for (input, expected) in testcases {
            let (exp_callname, exp_args) = expected;
            let argv = input.into_iter().map(|s| s.into());
            let (callname, args) = gather_cli_args(argv).unwrap();
            assert_eq!(callname, exp_callname);
            assert_eq!(args, exp_args);
        }
    }
}
