// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::{
    io::{BufRead as _, Seek},
    os::fd::IntoRawFd,
};

use anyhow::{Context as _, Result};
use cap_std::fs::Dir;
use cap_std_ext::cap_tempfile;
use clap::Parser;
use ostree_ext::gio;

use crate::{bwrap, ffi::BubblewrapMutability, impl_sealed_memfd};

#[derive(Debug, Parser)]
#[clap(rename_all = "kebab-case")]
/// Main options struct
struct Internals {
    #[clap(subcommand)]
    cmd: Cmd,
}

#[derive(Debug, Parser)]
#[clap(rename_all = "kebab-case")]
/// Options for invoking bubblewrap
struct BwrapOpts {
    /// Path to rootfs
    root: String,

    /// Arguments
    args: Vec<String>,
}

#[derive(Debug, Parser)]
#[clap(rename_all = "kebab-case")]
/// Options for invoking bubblewrap
struct BwrapScriptOpts {
    /// Path to rootfs
    root: String,

    /// Path to interpeter
    interp: String,

    /// Path to script
    script: String,
}

#[derive(Debug, clap::Subcommand)]
#[clap(rename_all = "kebab-case")]
/// Subcommands
enum Cmd {
    /// Invoke bubblewrap
    Bwrap(BwrapOpts),
    /// Invoke bubblewrap the same way rpm-ostree does for scripts.
    BwrapScript(BwrapScriptOpts),
}

impl BwrapOpts {
    fn run(self) -> Result<()> {
        let root = &Dir::open_ambient_dir(&self.root, cap_std::ambient_authority())?;
        let mut bwrap =
            bwrap::Bubblewrap::new_with_mutability(root, BubblewrapMutability::MutateFreely)?;
        bwrap.append_child_argv(self.args.iter().map(|s| s.as_str()));
        bwrap.run_inner(gio::Cancellable::NONE)?;
        Ok(())
    }
}

impl BwrapScriptOpts {
    fn run(self) -> Result<()> {
        let authority = cap_std::ambient_authority();
        let root = &Dir::open_ambient_dir(&self.root, authority)?;
        let mut bwrap =
            bwrap::Bubblewrap::new_with_mutability(root, BubblewrapMutability::MutateFreely)?;
        let td = Dir::open_ambient_dir("/var/tmp", authority)?;
        let mut output = cap_tempfile::TempFile::new_anonymous(&td)?.into_std();
        bwrap.append_child_arg(&self.interp);
        bwrap.take_stdout_and_stderr_fd(output.try_clone()?.into_raw_fd());
        let script = std::fs::read_to_string(self.script)?;
        let mfd = impl_sealed_memfd("script", script.as_bytes())?;
        bwrap.take_fd(mfd.into_raw_fd(), 5);
        bwrap.append_child_arg("/proc/self/fd/5");
        bwrap.run_inner(gio::Cancellable::NONE)?;
        output.seek(std::io::SeekFrom::Start(0))?;
        let output = std::io::BufReader::new(output);
        for line in output.lines() {
            let line = line.context("Reading line")?;
            println!("script: {line}");
        }
        Ok(())
    }
}

impl Cmd {
    fn run(self) -> Result<()> {
        match self {
            Cmd::Bwrap(args) => args.run(),
            Cmd::BwrapScript(args) => args.run(),
        }
    }
}

pub fn main(argv: &[&str]) -> Result<i32> {
    let opt = Internals::parse_from(argv.into_iter().skip(1));
    opt.cmd.run()?;
    Ok(0)
}
