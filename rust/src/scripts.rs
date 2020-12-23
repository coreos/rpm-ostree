/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use phf::phf_set;

/// Some RPM scripts we don't want to execute.  A notable example is the kernel ones;
/// we want rpm-ostree to own running dracut, installing the kernel to /boot etc.
/// Ideally more of these migrate out, e.g. in the future we should change the kernel
/// package to do nothing if `/run/ostree-booted` exists.
///
/// NOTE FOR GIT history: This list used to live in src/libpriv/rpmostree-script-gperf.gperf
static IGNORED_PKG_SCRIPTS: phf::Set<&'static str> = phf_set! {
    "glibc.prein",
    // We take over depmod/dracut etc.  It's `kernel` in C7 and kernel-core in F25+
    "kernel.posttrans",
    "kernel-core.posttrans",
    // Legacy workaround
    "glibc-headers.prein",
    // workaround for old bug?
    "coreutils.prein",
    // Looks like legacy...
    "ca-certificates.prein",
    "libgcc.post",
    "setup.post",
    "pinentry.prein",
    "fedora-release.posttrans",
    // These add the vagrant group which IMO is really
    // a libvirt-user group
    "vagrant.prein",
    "vagrant-libvirt.prein",
    // This one is in lua; the setup package seems to be generating the value.
    // Should probably be ported to a drop-in dir or a %posttrans
    "bash.post",
    // Seems to be another case of legacy workaround
    "gdb.prein",
    // Just does a daemon-reload  which we don't want offline
    "systemd.transfiletriggerin",
    // https://bugzilla.redhat.com/show_bug.cgi?id=1473402
    "man-db.transfiletriggerin",
    // https://src.fedoraproject.org/rpms/nfs-utils/pull-request/1
    "nfs-utils.post",
    // There is some totally insane stuff going on here in RHEL7
    "microcode_ctl.post",
    // https://bugzilla.redhat.com/show_bug.cgi?id=1199582
    "microcode_ctl.posttrans",
};

/// Returns true if we should simply ignore (not execute) an RPM script.
/// The format is <packagename>.<script>
pub(crate) fn script_is_ignored(pkg: &str, script: &str) -> bool {
    let script = script.trim_start_matches('%');
    let pkgscript = format!("{}.{}", pkg, script);
    IGNORED_PKG_SCRIPTS.contains(pkgscript.as_str())
}
