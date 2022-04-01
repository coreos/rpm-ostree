/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//! # Test utility functions
//!
//! This backs the hidden `rpm-ostree testutils` CLI.  Subject
//! to change.

use crate::cxxrsutil::*;
use anyhow::{Context, Result};
use cap_std::fs::FileType;
use cap_std::fs::{Dir, Permissions};
use cap_std_ext::cap_std;
use cap_std_ext::prelude::CapStdExtDirExt;
use cap_std_ext::rustix::fs::{MetadataExt, Mode};
use fn_error_context::context;
use glib::{ToVariant, Variant};
use ostree_ext::{gio, glib, ostree};
use rand::Rng;
use std::fs;
use std::fs::File;
use std::io::Write as IoWrite;
use std::os::unix::fs::FileExt as UnixFileExt;
use std::os::unix::prelude::PermissionsExt;
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
    /// All integration tests that require a booted machine, run as root, but are nondestructive
    IntegrationReadOnly,
    /// Run the C unit tests
    CUnits,
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
    dest: &Dir,
    notepath: &str,
    have_objcopy: bool,
) -> Result<()> {
    let mut destf = dest.create(name).context("Failed to open for write")?;
    destf.set_permissions(Permissions::from_mode(0o755))?;
    std::io::copy(f, &mut destf).context("Failed to copy")?;
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
    src: &Dir,
    dest: &Dir,
    percentage: u32,
    notepath: &str,
    have_objcopy: bool,
) -> Result<u32> {
    assert!(percentage > 0 && percentage <= 100);
    let mut mutated = 0;
    for entry in src.entries()? {
        let entry = entry?;
        if entry.file_type()? != FileType::file() {
            continue;
        }
        let meta = src.metadata(entry.file_name())?;
        let stmode = meta.mode();
        let mode = Mode::from_bits_truncate(stmode);
        // Must be executable
        if !mode.intersects(Mode::XUSR | Mode::XGRP | Mode::XOTH) {
            continue;
        }
        // Not suid
        if mode.intersects(Mode::SUID | Mode::SGID) {
            continue;
        }
        // Greater than 1k in size
        if meta.size() < 1024 {
            continue;
        }
        let mut f = src.open(entry.file_name())?.into_std();
        if !is_elf(&mut f)? {
            continue;
        }
        if !rand::thread_rng().gen_ratio(percentage, 100) {
            continue;
        }
        mutate_one_executable_to(&mut f, &entry.file_name(), dest, notepath, have_objcopy)
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
        let tempdir = Dir::open_ambient_dir(&tmp_rootfs, cap_std::ambient_authority())?;
        let binary_dirs = &["usr/bin", "usr/sbin", "usr/lib", "usr/lib64"];
        let rootfs = Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
        for v in binary_dirs {
            let v = *v;
            if let Some(src) = rootfs.open_dir_optional(v)? {
                tempdir.create_dir_all("usr")?;
                tempdir.create_dir_all(v)?;
                let dest = tempdir.open_dir(v)?;
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

    let params = Variant::from_tuple(&[true.to_variant()]);
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
    let reply = reply.child_value(0);
    // Unwrap safety: We validated the (s) above.
    let reply = reply.str().unwrap();
    let cow = "🐄\n";
    assert_eq!(reply, cow);
    println!("ok {}", cow.trim());
    Ok(())
}

pub(crate) fn testutils_entrypoint(args: Vec<String>) -> CxxResult<()> {
    let opt = Opt::from_iter(args.iter());
    match opt {
        Opt::GenerateSyntheticUpgrade(ref opts) => update_os_tree(opts)?,
        Opt::IntegrationReadOnly => integration_read_only()?,
        Opt::CUnits => crate::ffi::c_unit_tests()?,
        Opt::Moo => test_moo()?,
    };
    Ok(())
}

fn test_pkg_variants(repo: &ostree::Repo, booted_commit: &str) -> Result<()> {
    let repo = repo.gobj_rewrap();
    let cancellable = gio::Cancellable::new();
    let cancellable = cancellable.gobj_rewrap();
    // This returns an a(sssss) as a raw value
    let v = crate::ffi::package_variant_list_for_commit(repo, booted_commit, cancellable)?;
    let v: glib::Variant = unsafe { glib::translate::from_glib_full(v as *mut _) };
    for p in v.iter() {
        let n = p.child_value(0);
        let n = n.str().unwrap();
        if n == "ostree" {
            return Ok(());
        }
    }

    anyhow::bail!("Failed to find ostree package")
}

fn integration_read_only() -> Result<()> {
    let cancellable = gio::NONE_CANCELLABLE;
    let sysroot = &ostree::Sysroot::new_default();
    sysroot.load(cancellable)?;
    let repo = &sysroot.repo().unwrap();
    let booted = &sysroot.require_booted_deployment()?;
    let booted_commit = &booted.csum().expect("csum");
    let booted_commit = booted_commit.as_str();
    sysroot.load(cancellable)?;
    validate_parse_status()?;
    test_pkg_variants(repo, booted_commit)?;
    println!("ok integration read only");
    Ok(())
}
