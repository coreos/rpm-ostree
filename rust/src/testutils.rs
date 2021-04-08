/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//! # Test utility functions
//!
//! This backs the hidden `rpm-ostree testutils` CLI.  Subject
//! to change.

use crate::{cxxrsutil::*, variant_utils};
use anyhow::{Context, Result};
use fn_error_context::context;
use glib::ToVariant;
use openat_ext::{FileExt, OpenatDirExt};
use rand::Rng;
use std::fs;
use std::fs::File;
use std::io::Write as IoWrite;
use std::os::unix::fs::FileExt as UnixFileExt;
use std::path::Path;
use std::process::Command;
use structopt::StructOpt;

#[derive(Debug, StructOpt)]
struct SyntheticUpgradeOpts {
    #[structopt(long)]
    repo: String,

    #[structopt(long = "srcref")]
    src_ref: Option<String>,

    #[structopt(long = "ref")]
    ostref: String,

    #[structopt(long, default_value = "30")]
    percentage: u32,

    #[structopt(long)]
    commit_version: Option<String>,
}

#[derive(Debug, StructOpt)]
#[structopt(name = "testutils")]
#[structopt(rename_all = "kebab-case")]
enum Opt {
    /// Generate an OS update by changing ELF files
    GenerateSyntheticUpgrade(SyntheticUpgradeOpts),
    /// Validate that we can parse the output of `rpm-ostree status --json`.
    ValidateParseStatus,
    /// Test that we can 🐄
    Moo,
}

/// Returns `true` if a file is ELF; see https://en.wikipedia.org/wiki/Executable_and_Linkable_Format
pub(crate) fn is_elf(f: &mut File) -> Result<bool> {
    let mut buf = [0; 5];
    let n = f.read_at(&mut buf, 0)?;
    if n < buf.len() {
        anyhow::bail!("Failed to read expected {} bytes", buf.len());
    }
    Ok(buf[0] == 0x7F && &buf[1..4] == b"ELF")
}

pub(crate) fn mutate_one_executable_to(
    f: &mut File,
    name: &std::ffi::OsStr,
    dest: &openat::Dir,
    notepath: &str,
    have_objcopy: bool,
) -> Result<()> {
    let mut destf = dest
        .write_file(name, 0o755)
        .context("Failed to open for write")?;
    f.copy_to(&destf).context("Failed to copy")?;
    if have_objcopy {
        std::mem::drop(destf);
        let r = Command::new("objcopy")
            .arg(format!("--add-section=.note.coreos-synthetic={}", notepath))
            .status()?;
        if !r.success() {
            anyhow::bail!("objcopy failed: {:?}", r)
        }
    } else {
        // ELF is OK with us just appending some junk
        let extra = rand::thread_rng()
            .sample_iter(&rand::distributions::Alphanumeric)
            .take(10)
            .collect::<Vec<u8>>();
        destf
            .write_all(&extra)
            .context("Failed to append extra data")?;
    }

    Ok(())
}

/// Find ELF files in the srcdir, write new copies to dest (only percentage)
pub(crate) fn mutate_executables_to(
    src: &openat::Dir,
    dest: &openat::Dir,
    percentage: u32,
    notepath: &str,
    have_objcopy: bool,
) -> Result<u32> {
    use nix::sys::stat::Mode as NixMode;
    assert!(percentage > 0 && percentage <= 100);
    let mut mutated = 0;
    for entry in src.list_dir(".")? {
        let entry = entry?;
        if src.get_file_type(&entry)? != openat::SimpleType::File {
            continue;
        }
        let meta = src.metadata(entry.file_name())?;
        let st = meta.stat();
        let mode = NixMode::from_bits_truncate(st.st_mode);
        // Must be executable
        if !mode.intersects(NixMode::S_IXUSR | NixMode::S_IXGRP | NixMode::S_IXOTH) {
            continue;
        }
        // Not suid
        if mode.intersects(NixMode::S_ISUID | NixMode::S_ISGID) {
            continue;
        }
        // Greater than 1k in size
        if st.st_size < 1024 {
            continue;
        }
        let mut f = src.open_file(entry.file_name())?;
        if !is_elf(&mut f)? {
            continue;
        }
        if !rand::thread_rng().gen_ratio(percentage, 100) {
            continue;
        }
        mutate_one_executable_to(&mut f, entry.file_name(), dest, notepath, have_objcopy)
            .with_context(|| format!("Failed updating {:?}", entry.file_name()))?;
        mutated += 1;
    }
    Ok(mutated)
}

// Note this function is copied from https://github.com/ostreedev/ostree/blob/364556b8ae30d1a70179a49e5238c8f5e85f8776/tests/inst/src/treegen.rs#L117
// The ostree version may later use this one.
/// Given an ostree ref, use the running root filesystem as a source, update
/// `percentage` percent of binary (ELF) files
#[context("Generating synthetic ostree update")]
fn update_os_tree(opts: &SyntheticUpgradeOpts) -> Result<()> {
    // A new mount namespace should have been created for us
    let r = Command::new("mount")
        .args(&["-o", "remount,rw", "/sysroot"])
        .status()?;
    if !r.success() {
        anyhow::bail!("Failed to remount /sysroot");
    }
    assert!(opts.percentage > 0 && opts.percentage <= 100);
    let repo_path = Path::new(opts.repo.as_str());
    let tempdir = tempfile::tempdir_in(repo_path.join("tmp"))?;
    let tmp_rootfs = tempdir.path().join("rootfs");
    fs::create_dir(&tmp_rootfs)?;
    let notepath = tempdir.path().join("note");
    fs::write(&notepath, "Synthetic upgrade")?;
    let mut mutated = 0;
    // TODO run this as a container image, or (much more heavyweight)
    // depend on https://lib.rs/crates/goblin
    let have_objcopy = Path::new("/usr/bin/objcopy").exists();
    {
        let tempdir = openat::Dir::open(&tmp_rootfs)?;
        let binary_dirs = &["usr/bin", "usr/sbin", "usr/lib", "usr/lib64"];
        let rootfs = openat::Dir::open("/")?;
        for v in binary_dirs {
            let v = *v;
            if let Some(src) = rootfs.sub_dir_optional(v)? {
                tempdir.ensure_dir("usr", 0o755)?;
                tempdir.ensure_dir(v, 0o755)?;
                let dest = tempdir.sub_dir(v)?;
                mutated += mutate_executables_to(
                    &src,
                    &dest,
                    opts.percentage,
                    notepath.to_str().unwrap(),
                    have_objcopy,
                )
                .with_context(|| format!("Replacing binaries in {}", v))?;
            }
        }
    }
    assert!(mutated > 0);
    println!("Mutated ELF files: {}", mutated);
    let src_ref = opts
        .src_ref
        .as_deref()
        .unwrap_or_else(|| opts.ostref.as_str());
    let mut cmd = Command::new("ostree");
    cmd.arg(format!("--repo={}", repo_path.to_str().unwrap()))
        .args(&["commit", "--consume", "-b"])
        .arg(opts.ostref.as_str())
        .args(&[
            format!("--base={}", src_ref),
            format!("--tree=dir={}", tmp_rootfs.to_str().unwrap()),
        ])
        .args(&[
            "--owner-uid=0",
            "--owner-gid=0",
            "--selinux-policy-from-base",
            "--link-checkout-speedup",
            "--no-bindings",
            "--no-xattrs",
        ]);
    if let Some(v) = opts.commit_version.as_ref() {
        cmd.arg(format!("--add-metadata-string=version={}", v));
    }
    let r = cmd.status()?;
    if !r.success() {
        anyhow::bail!("Failed to commit updated content: {:?}", r)
    }
    Ok(())
}

// We always expect to be in a booted deployment.  The real goal here
// is to ensure that everything output from status --json in our
// test suite can be parsed by our client side bindings.
//
// In the future we'll switch the client API to have like
// query_status_deny_unknown_fields() which will force us
// to update the client bindings when adding new fields.
fn validate_parse_status() -> Result<()> {
    let c = rpmostree_client::CliClient::new("tests");
    let s = c.query_status().map_err(anyhow::Error::msg)?;
    assert_ne!(s.deployments.len(), 0);
    Ok(())
}

fn test_moo() -> Result<()> {
    crate::ffi::client_require_root()?;

    let mut client_conn = crate::ffi::new_client_connection()?;
    let mut bus_conn = client_conn.pin_mut().get_connection();
    let bus_conn = bus_conn.gobj_wrap();

    let params = crate::variant_utils::new_variant_tuple(&[true.to_variant()]);
    let reply = &bus_conn.call_sync(
        Some("org.projectatomic.rpmostree1"),
        "/org/projectatomic/rpmostree1/fedora_coreos",
        "org.projectatomic.rpmostree1.OSExperimental",
        "Moo",
        Some(&params),
        Some(glib::VariantTy::new("(s)").unwrap()),
        gio::DBusCallFlags::NONE,
        -1,
        gio::NONE_CANCELLABLE,
    )?;
    let reply = variant_utils::variant_tuple_get(reply, 0).unwrap();
    // Unwrap safety: We validated the (s) above.
    let reply = reply.get_str().unwrap();
    let cow = "🐄\n";
    assert_eq!(reply, cow);
    println!("ok {}", cow.trim());
    Ok(())
}

pub(crate) fn testutils_entrypoint(args: Vec<String>) -> CxxResult<()> {
    let opt = Opt::from_iter(args.iter());
    match opt {
        Opt::GenerateSyntheticUpgrade(ref opts) => update_os_tree(opts)?,
        Opt::ValidateParseStatus => validate_parse_status()?,
        Opt::Moo => test_moo()?,
    };
    Ok(())
}
