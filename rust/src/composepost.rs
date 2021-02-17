/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

//! Code run server side to "postprocess"
//! a filesystem tree (usually containing mostly RPMs) in
//! order to prepare it as an OSTree commit.

use crate::cxxrsutil::CxxResult;
use anyhow::Result;
use openat_ext::OpenatDirExt;
use rayon::prelude::*;
use std::io;
use std::io::{BufRead, Write};
use std::path::Path;

/* See rpmostree-core.h */
const RPMOSTREE_RPMDB_LOCATION: &str = "usr/share/rpm";

// rpm-ostree uses /home → /var/home by default as generated by our
// rootfs; we don't expect people to change this.  Let's be nice
// and also fixup the $HOME entries generated by `useradd` so
// that `~` shows up as expected in shells, etc.
//
// https://github.com/coreos/fedora-coreos-config/pull/18
// https://pagure.io/workstation-ostree-config/pull-request/121
// https://discussion.fedoraproject.org/t/adapting-user-home-in-etc-passwd/487/6
// https://github.com/justjanne/powerline-go/issues/94
fn postprocess_useradd(rootfs_dfd: &openat::Dir) -> Result<()> {
    let path = Path::new("usr/etc/default/useradd");
    if let Some(f) = rootfs_dfd.open_file_optional(path)? {
        rootfs_dfd.write_file_with(&path, 0o644, |bufw| -> Result<_> {
            let f = io::BufReader::new(&f);
            for line in f.lines() {
                let line = line?;
                if !line.starts_with("HOME=") {
                    bufw.write_all(line.as_bytes())?;
                } else {
                    bufw.write_all(b"HOME=/var/home")?;
                }
                bufw.write_all(b"\n")?;
            }
            Ok(())
        })?;
    }
    Ok(())
}

// We keep hitting issues with the ostree-remount preset not being
// enabled; let's just do this rather than trying to propagate the
// preset everywhere.
fn postprocess_presets(rootfs_dfd: &openat::Dir) -> Result<()> {
    let wantsdir = "usr/lib/systemd/system/multi-user.target.wants";
    rootfs_dfd.ensure_dir_all(wantsdir, 0o755)?;
    for service in &["ostree-remount.service", "ostree-finalize-staged.path"] {
        let target = format!("../{}", service);
        let loc = Path::new(wantsdir).join(service);
        rootfs_dfd.symlink(&loc, target)?;
    }
    Ok(())
}

// We keep hitting issues with the ostree-remount preset not being
// enabled; let's just do this rather than trying to propagate the
// preset everywhere.
fn postprocess_rpm_macro(rootfs_dfd: &openat::Dir) -> Result<()> {
    let rpm_macros_dir = "usr/lib/rpm/macros.d";
    rootfs_dfd.ensure_dir_all(rpm_macros_dir, 0o755)?;
    let rpm_macros_dfd = rootfs_dfd.sub_dir(rpm_macros_dir)?;
    rpm_macros_dfd.write_file_with("macros.rpm-ostree", 0o644, |w| -> Result<()> {
        w.write_all(b"%_dbpath /")?;
        w.write_all(RPMOSTREE_RPMDB_LOCATION.as_bytes())?;
        Ok(())
    })?;
    Ok(())
}

// This function does two things: (1) make sure there is a /home --> /var/home substitution rule,
// and (2) make sure there *isn't* a /var/home -> /home substition rule. The latter check won't
// technically be needed once downstreams have:
// https://src.fedoraproject.org/rpms/selinux-policy/pull-request/14
fn postprocess_subs_dist(rootfs_dfd: &openat::Dir) -> Result<()> {
    let path = Path::new("usr/etc/selinux/targeted/contexts/files/file_contexts.subs_dist");
    if let Some(f) = rootfs_dfd.open_file_optional(path)? {
        rootfs_dfd.write_file_with(&path, 0o644, |w| -> Result<()> {
            let f = io::BufReader::new(&f);
            for line in f.lines() {
                let line = line?;
                if line.starts_with("/var/home ") {
                    w.write_all(b"# https://github.com/projectatomic/rpm-ostree/pull/1754\n")?;
                    w.write_all(b"# ")?;
                }
                w.write_all(line.as_bytes())?;
                w.write_all(b"\n")?;
            }
            w.write_all(b"# https://github.com/projectatomic/rpm-ostree/pull/1754\n")?;
            w.write_all(b"/home /var/home")?;
            w.write_all(b"\n")?;
            Ok(())
        })?;
    }
    Ok(())
}

// This function is called from rpmostree_postprocess_final(); think of
// it as the bits of that function that we've chosen to implement in Rust.
pub(crate) fn compose_postprocess_final(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs_dfd = crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let tasks = [
        postprocess_useradd,
        postprocess_presets,
        postprocess_subs_dist,
        postprocess_rpm_macro,
    ];
    Ok(tasks.par_iter().try_for_each(|f| f(&rootfs_dfd))?)
}
