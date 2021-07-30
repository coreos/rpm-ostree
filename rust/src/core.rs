//! Code mirroring rpmostree-core.cxx which is the shared "core"
//! binding of rpm and ostree, used by both client-side layering/overrides
//! and server side composes.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::CxxResult;
use crate::ffiutil;
use anyhow::Result;
use ffiutil::*;
use fn_error_context::context;
use openat_ext::OpenatDirExt;

/// The binary forked from useradd that pokes the sss cache.
/// It spews warnings (and sometimes fatal errors) when used
/// in a non-systemd container (default treecompose side) so
/// we temporarily remove it.  https://github.com/SSSD/sssd/issues/5687
const SSS_CACHE_PATH: &str = "usr/sbin/sss_cache";
// Also neuter systemctl - at least glusterfs for example calls `systemctl start`
// in its %post which both violates Fedora policy and also will not
// work with the rpm-ostree model.
// See also https://github.com/projectatomic/rpm-ostree/issues/550
// See also the SYSTEMD_OFFLINE bits in rpmostree-scripts.c; at some
// point in the far future when we don't support RHEL/CentOS7 we can drop
// our wrapper script.  If we remember.
const SYSTEMCTL_PATH: &str = "usr/bin/systemctl";
const SYSTEMCTL_WRAPPER: &[u8] = include_bytes!("../../src/libpriv/systemctl-wrapper.sh");

/// Guard for running logic in a context with temporary /etc.
///
/// We have a messy dance in dealing with /usr/etc and /etc; the
/// current model is basically to have it be /etc whenever we're running
/// any code.
#[derive(Debug)]
pub struct TempEtcGuard {
    rootfs: openat::Dir,
    renamed_etc: bool,
}

/// Detect if we have /usr/etc and no /etc, and rename if so.
pub(crate) fn prepare_tempetc_guard(rootfs: i32) -> CxxResult<Box<TempEtcGuard>> {
    let rootfs = ffi_view_openat_dir(rootfs);
    let has_etc = rootfs.exists("etc")?;
    let mut renamed_etc = false;
    if !has_etc && rootfs.exists("usr/etc")? {
        // In general now, we place contents in /etc when running scripts
        rootfs.local_rename("usr/etc", "etc")?;
        // But leave a compat symlink, as we used to bind mount, so scripts
        // could still use that too.
        rootfs.symlink("usr/etc", "../etc")?;
        renamed_etc = true;
    }
    Ok(Box::new(TempEtcGuard {
        rootfs,
        renamed_etc,
    }))
}

impl TempEtcGuard {
    /// Remove the temporary /etc, and destroy the guard.
    pub(crate) fn undo(&self) -> CxxResult<()> {
        if self.renamed_etc {
            /* Remove the symlink and swap back */
            self.rootfs.remove_file("usr/etc")?;
            self.rootfs.local_rename("etc", "usr/etc")?;
        }
        Ok(())
    }
}

/// Run the standard `depmod` utility.
pub(crate) fn run_depmod(rootfs_dfd: i32, kver: &str, unified_core: bool) -> CxxResult<()> {
    let args: Vec<_> = vec!["depmod", "-a", kver]
        .into_iter()
        .map(|s| s.to_string())
        .collect();
    let _ = crate::bwrap::bubblewrap_run_sync(rootfs_dfd, &args, false, unified_core)?;
    Ok(())
}

/// Infer whether refspec is a ctonainer image reference.
pub(crate) fn is_container_image_reference(refspec: &str) -> bool {
    // Currently, we are simply relying on the fact that there cannot be multiple colons
    // or the `@` symbol in TYPE_OSTREE or TYPE_COMMIT refspecs. We may want a more robust
    // and reliable way of determining the refspec type in the future, as some container
    // transports may possibly not contain colons.
    // https://github.com/coreos/rpm-ostree/issues/2909#issuecomment-868151689
    refspec.split(':').nth(2).is_some() || refspec.contains('@')
}

/// Perform reversible filesystem transformations necessary before we execute scripts.
pub(crate) struct FilesystemScriptPrep {
    rootfs: openat::Dir,
}

pub(crate) fn prepare_filesystem_script_prep(rootfs: i32) -> CxxResult<Box<FilesystemScriptPrep>> {
    let rootfs = ffi_view_openat_dir(rootfs);
    Ok(FilesystemScriptPrep::new(rootfs)?)
}

/// Using the Rust log infrastructure, print the treefile.
pub(crate) fn log_treefile(tf: &crate::treefile::Treefile) {
    tracing::debug!("Using treefile:\n{}", tf.get_json_string());
}

impl FilesystemScriptPrep {
    /// Filesystem paths that we rename out of the way if present
    const OPTIONAL_PATHS: &'static [&'static str] = &[SSS_CACHE_PATH];
    const REPLACE_OPTIONAL_PATHS: &'static [(&'static str, &'static [u8])] =
        &[(SYSTEMCTL_PATH, SYSTEMCTL_WRAPPER)];

    fn saved_name(name: &str) -> String {
        format!("{}.rpmostreesave", name)
    }

    #[context("Preparing filesystem for scripts")]
    pub(crate) fn new(rootfs: openat::Dir) -> Result<Box<Self>> {
        for &path in Self::OPTIONAL_PATHS {
            rootfs.local_rename_optional(path, &Self::saved_name(path))?;
        }
        for &(path, contents) in Self::REPLACE_OPTIONAL_PATHS {
            if rootfs.local_rename_optional(path, &Self::saved_name(path))? {
                rootfs.write_file_contents(path, 0o755, contents)?;
            }
        }
        Ok(Box::new(Self { rootfs }))
    }

    /// Undo the filesystem changes.
    #[context("Undoing prep filesystem for scripts")]
    pub(crate) fn undo(&self) -> CxxResult<()> {
        for &path in Self::OPTIONAL_PATHS
            .iter()
            .chain(Self::REPLACE_OPTIONAL_PATHS.iter().map(|x| &x.0))
        {
            self.rootfs
                .local_rename_optional(&Self::saved_name(path), path)?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use anyhow::Result;
    use std::os::unix::prelude::*;

    #[test]
    fn etcguard() -> Result<()> {
        let td = tempfile::tempdir()?;
        let d = openat::Dir::open(td.path())?;
        let g = super::prepare_tempetc_guard(d.as_raw_fd())?;
        g.undo()?;
        d.ensure_dir_all("usr/etc/foo", 0o755)?;
        assert!(!d.exists("etc/foo")?);
        let g = super::prepare_tempetc_guard(d.as_raw_fd())?;
        assert!(d.exists("etc/foo")?);
        g.undo()?;
        assert!(!d.exists("etc")?);
        assert!(d.exists("usr/etc/foo")?);
        Ok(())
    }

    #[test]
    fn rootfs() -> Result<()> {
        let td = tempfile::tempdir()?;
        let d = openat::Dir::open(td.path())?;
        // The no-op case
        {
            let g = super::prepare_filesystem_script_prep(d.as_raw_fd())?;
            g.undo()?;
        }
        d.ensure_dir_all("usr/bin", 0o755)?;
        d.ensure_dir_all("usr/sbin", 0o755)?;
        d.write_file_contents(super::SSS_CACHE_PATH, 0o755, "sss binary")?;
        let original_systemctl = "original systemctl";
        d.write_file_contents(super::SYSTEMCTL_PATH, 0o755, original_systemctl)?;
        // Replaced systemctl
        {
            assert!(d.exists(super::SSS_CACHE_PATH)?);
            let g = super::prepare_filesystem_script_prep(d.as_raw_fd())?;
            assert!(!d.exists(super::SSS_CACHE_PATH)?);
            let contents = d.read_to_string(super::SYSTEMCTL_PATH)?;
            assert_eq!(contents.as_bytes(), super::SYSTEMCTL_WRAPPER);
            g.undo()?;
            let contents = d.read_to_string(super::SYSTEMCTL_PATH)?;
            assert_eq!(contents, original_systemctl);
            assert!(d.exists(super::SSS_CACHE_PATH)?);
        }
        Ok(())
    }

    #[test]
    fn test_is_container_image_reference() -> Result<()> {
        use super::is_container_image_reference;

        let refspec_type_checksum =
            "ee10f8e7ef638d78ba9a9596665067f58021624118875cc4079568da6c63efb0";
        assert!(!is_container_image_reference(refspec_type_checksum));

        let refspec_type_ostree_with_remote = "fedora:fedora/x86_64/coreos/testing-devel";
        assert!(!is_container_image_reference(
            refspec_type_ostree_with_remote
        ));
        let refspec_type_ostree = "fedora/x86_64/coreos/foo-branch";
        assert!(!is_container_image_reference(refspec_type_ostree));

        const REFSPEC_TYPE_CONTAINER: &[&str] = &[
            "containers-storage:localhost/fcos:latest",
            "docker://quay.io/test-repository/os:version1",
            "registry:docker.io/test-repository/os:latest",
            "registry:customhostname.com:8080/test-repository/os:latest",
            "docker://quay.io/test-repository/os@sha256:6006dca86c2dc549c123ff4f1dcbe60105fb05886531c93a3351ebe81dbe772f",
        ];

        for refspec in REFSPEC_TYPE_CONTAINER {
            assert!(is_container_image_reference(refspec));
        }

        Ok(())
    }
}
