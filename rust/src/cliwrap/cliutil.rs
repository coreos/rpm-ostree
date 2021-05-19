// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;
use nix::sys::statvfs;
use std::os::unix::process::CommandExt;
use std::{path, thread, time};

use crate::cliwrap;

/// Returns true if the current process is booted via ostree.
pub fn is_ostree_booted() -> bool {
    path::Path::new("/run/ostree-booted").exists()
}

/// Returns true if /usr is not a read-only bind mount
pub fn is_unlocked() -> Result<bool> {
    Ok(!statvfs::statvfs("/usr")?
        .flags()
        .contains(statvfs::FsFlags::ST_RDONLY))
}

/// Returns true if the current process is running as root.
pub fn am_privileged() -> bool {
    nix::unistd::getuid() == nix::unistd::Uid::from_raw(0)
}

/// Return the absolute path to the underlying wrapped binary
fn get_real_binary_path(bin_name: &str) -> String {
    format!("/{}/{}", cliwrap::CLIWRAP_DESTDIR, bin_name)
}

/// Wrapper for execv which accepts strings
pub fn exec_real_binary<T: AsRef<str> + std::fmt::Display>(bin_name: T, argv: &[T]) -> Result<()> {
    let bin_name = bin_name.as_ref();
    let real_bin = get_real_binary_path(bin_name);
    let mut proc = std::process::Command::new(real_bin);
    proc.args(argv.iter().map(|s| s.as_ref()));
    Err(proc.exec().into())
}

/// Run a subprocess synchronously as user `bin` (dropping all capabilities).
pub fn run_unprivileged<T: AsRef<str>>(
    with_warning: bool,
    target_bin: &str,
    argv: &[T],
) -> Result<()> {
    // `setpriv` is in util-linux; we could do this internally, but this is easier.
    let setpriv_argv = &[
        "setpriv",
        "--no-new-privs",
        "--reuid=bin",
        "--regid=bin",
        "--init-groups",
        "--bounding-set",
        "-all",
        "--",
    ];

    let argv: Vec<&str> = argv.iter().map(AsRef::as_ref).collect();
    let drop_privileges = am_privileged();
    let app_name = "rpm-ostree";
    if with_warning {
        let delay_s = 5;
        eprintln!(
            "{name}: NOTE: This system is ostree based.",
            name = app_name
        );
        if drop_privileges {
            eprintln!(
                r#"{name}: Dropping privileges as `{bin}` was executed with not "known safe" arguments."#,
                name = app_name,
                bin = target_bin
            );
        } else {
            eprintln!(
                r#"{name}: Wrapped binary "{bin}" was executed with not "known safe" arguments."#,
                name = app_name,
                bin = target_bin
            );
        }
        eprintln!(
            r##"{name}: You may invoke the real `{bin}` binary in `/{wrap_destdir}/{bin}`.
{name}: Continuing execution in {delay} seconds.
"##,
            name = app_name,
            wrap_destdir = cliwrap::CLIWRAP_DESTDIR,
            bin = target_bin,
            delay = delay_s,
        );
        thread::sleep(time::Duration::from_secs(delay_s));
    }

    if drop_privileges {
        let real_bin = get_real_binary_path(target_bin);
        let mut proc = std::process::Command::new("setpriv");
        proc.args(setpriv_argv);
        proc.arg(real_bin);
        proc.args(argv);
        proc.current_dir("/");
        Err(proc.exec().into())
    } else {
        exec_real_binary(target_bin, &argv)
    }
}
