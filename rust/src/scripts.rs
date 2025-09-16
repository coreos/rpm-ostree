//! Code corresponding to rpmostree-scripts.cxx which deals with
//! RPM scripts, and is in process of being converted to Rust.

/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use phf::phf_set;

/// If we're not using boot-location: kernel-install, then we take over the
/// kernel RPM scripts.
///
/// NOTE FOR GIT history: This list used to live in src/libpriv/rpmostree-script-gperf.gperf
static IGNORED_KERNEL_SCRIPTS: phf::Set<&'static str> = phf_set! {
    // We take over depmod/dracut etc.  It's `kernel` in C7 and kernel-core in F25+
    // XXX: we should probably change this to instead ignore based on the kernel virtual Provides
    "kernel.posttrans",
    "kernel-core.posttrans",
    "kernel-modules.posttrans",
    "kernel-redhat-core.posttrans",
    "kernel-redhat-modules.posttrans",
    "kernel-debug-core.posttrans",
    "kernel-debug-modules.posttrans",
    "kernel-redhat-debug-core.posttrans",
    "kernel-redhat-debug-modules.posttrans",
    "kernel-automotive-core.posttrans",
    "kernel-automotive-modules.posttrans",
    "kernel-automotive-debug-core.posttrans",
    "kernel-automotive-debug-modules.posttrans",
    "kernel-rt-core.posttrans",
    "kernel-rt-modules.posttrans",
    "kernel-rt-debug-core.posttrans",
    "kernel-rt-debug-modules.posttrans",
    "kernel-16k-core.posttrans",
    "kernel-16k-modules.posttrans",
    "kernel-16k-debug-core.posttrans",
    "kernel-16k-debug-modules.posttrans",
    "kernel-64k-core.posttrans",
    "kernel-64k-modules.posttrans",
    "kernel-64k-debug-core.posttrans",
    "kernel-64k-debug-modules.posttrans",
    // Additionally ignore posttrans scripts for the Oracle Linux `kernel-uek` and `kernel-uek-core` packages
    "kernel-uek.posttrans",
    "kernel-uek-core.posttrans",
    // Additionally ignore posttrans scripts for ELRepo's `kernel-lt` and `kernel-ml` packages
    "kernel-lt.posttrans",
    "kernel-lt-core.posttrans",
    "kernel-lt-modules.posttrans",
    "kernel-ml.posttrans",
    "kernel-ml-core.posttrans",
    "kernel-ml-modules.posttrans",
};

static IGNORED_PKG_SCRIPTS: phf::Set<&'static str> = phf_set! {
    "glibc.prein",
    // Legacy workaround
    "glibc-headers.prein",
    // workaround for old bug?
    "coreutils.prein",
    // Looks like legacy...
    "ca-certificates.prein",
    "libgcc.post",
    // The entire filesystem package is insane.  Every hack in there would go
    // away if we just taught traditional yum/dnf to install it first, in
    // the same way we do in rpm-ostree.
    "filesystem.posttrans",
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
    // See https://gitlab.com/fedora/bootc/tracker/-/issues/29 - we don't need
    // any of this.
    "filesystem.transfiletriggerin",
    // https://src.fedoraproject.org/rpms/nfs-utils/pull-request/1
    "nfs-utils.post",
    // There is some totally insane stuff going on here in RHEL7
    "microcode_ctl.post",
    // https://bugzilla.redhat.com/show_bug.cgi?id=1199582
    "microcode_ctl.posttrans",
    // /usr/lib/sysimage/rpm is read only when we run scripts
    "rpm.posttrans",
};

/// Returns true if we should simply ignore (not execute) an RPM script.
/// The format is <packagename>.<script>
pub(crate) fn script_is_ignored(pkg: &str, script: &str, use_kernel_install: bool) -> bool {
    let script = script.trim_start_matches('%');
    let pkgscript = format!("{}.{}", pkg, script);
    if IGNORED_PKG_SCRIPTS.contains(&pkgscript) {
        return true;
    }
    if !use_kernel_install {
        return IGNORED_KERNEL_SCRIPTS.contains(&pkgscript);
    }
    return false;
}

#[cfg(test)]
mod test {
    use crate::script_is_ignored;

    #[test]
    fn test_script_is_ignored() {
        let ignored = ["microcode_ctl.post", "vagrant.prein"];
        let not_ignored = ["foobar.post", "systemd.posttrans"];
        let kernel_ignored = ["kernel-automotive-core.posttrans"];
        for v in ignored {
            let (pkg, script) = v.split_once('.').unwrap();
            assert!(script_is_ignored(pkg, script, false));
            assert!(script_is_ignored(pkg, script, true));
        }
        for v in not_ignored {
            let (pkg, script) = v.split_once('.').unwrap();
            assert!(!script_is_ignored(pkg, script, false));
            assert!(!script_is_ignored(pkg, script, true));
        }
        for v in kernel_ignored {
            let (pkg, script) = v.split_once('.').unwrap();
            assert!(script_is_ignored(pkg, script, false));
            assert!(!script_is_ignored(pkg, script, true));
        }
    }
}
