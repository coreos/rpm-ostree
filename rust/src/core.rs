//! Code mirroring rpmostree-core.cxx which is the shared "core"
//! binding of rpm and ostree, used by both client-side layering/overrides
//! and server side composes.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use crate::ffiutil;
use crate::kernel_install::is_ostree_layout;
use anyhow::Context;
use anyhow::{anyhow, Result};
use camino::Utf8Path;
use cap_std::fs::{Dir, Permissions, PermissionsExt};
use cap_std_ext::cap_std;
use cap_std_ext::prelude::CapStdExtDirExt;
use ffiutil::*;
use fn_error_context::context;
use glib::prelude::StaticVariantType;
use libdnf_sys::*;
use ostree_ext::container::OstreeImageReference;
use ostree_ext::glib;
use ostree_ext::oci_spec::distribution::Reference as OciReference;
use ostree_ext::ostree;
use std::fs::File;
use std::io::{BufReader, Read};
use std::os::unix::io::{AsRawFd, FromRawFd};

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

// Intercept commands for automatic sysusers.d fragment generation.
const GROUPADD_PATH: &str = "usr/sbin/groupadd";
const GROUPADD_WRAPPER: &[u8] = include_bytes!("../../src/libpriv/groupadd-wrapper.sh");
const USERADD_PATH: &str = "usr/sbin/useradd";
const USERADD_WRAPPER: &[u8] = include_bytes!("../../src/libpriv/useradd-wrapper.sh");
const USERMOD_PATH: &str = "usr/sbin/usermod";
const USERMOD_WRAPPER: &[u8] = include_bytes!("../../src/libpriv/usermod-wrapper.sh");
const KERNEL_INSTALL_PATH: &str = "usr/bin/kernel-install";
const KERNEL_INSTALL_WRAPPER: &[u8] = include_bytes!("../../src/libpriv/kernel-install-wrapper.sh");

const RPMOSTREE_CORE_STAGED_RPMS_DIR: &str = "rpm-ostree/staged-rpms";

pub(crate) const OSTREE_BOOTED: &str = "/run/ostree-booted";

/// Guard for running logic in a context with temporary /etc.
///
/// We have a messy dance in dealing with /usr/etc and /etc; the
/// current model is basically to have it be /etc whenever we're running
/// any code.
#[derive(Debug)]
pub struct TempEtcGuard {
    rootfs: Dir,
    renamed_etc: bool,
}

/// Detect if we have /usr/etc and no /etc, and rename if so.
pub(crate) fn prepare_tempetc_guard(rootfs: i32) -> CxxResult<Box<TempEtcGuard>> {
    let rootfs = unsafe { ffiutil::ffi_dirfd(rootfs)? };
    let has_etc = rootfs.try_exists("etc")?;
    let mut renamed_etc = false;
    if !has_etc && rootfs.try_exists("usr/etc")? {
        // In general now, we place contents in /etc when running scripts
        rootfs.rename("usr/etc", &rootfs, "etc")?;
        // But leave a compat symlink, as we used to bind mount, so scripts
        // could still use that too.
        rootfs.symlink("../etc", "usr/etc")?;
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
            self.rootfs.rename("etc", &self.rootfs, "usr/etc")?;
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
    let _ = crate::bwrap::bubblewrap_run_sync(
        rootfs_dfd,
        &args,
        false,
        crate::ffi::BubblewrapMutability::for_unified_core(unified_core),
    )?;
    Ok(())
}

/// Infer whether string is a container image reference.
pub(crate) fn is_container_image_reference(refspec: &str) -> bool {
    // this is slightly less efficient than calling just try_from(), but meh...
    refspec_classify(refspec) == crate::ffi::RefspecType::Container
}

/// Infer whether string is a container image reference.
pub(crate) fn is_container_image_digest_reference(refspec: &str) -> bool {
    OstreeImageReference::try_from(refspec)
        .and_then(|ostree_imgref| {
            ostree_imgref
                .imgref
                .name
                .parse::<OciReference>()
                .map_err(anyhow::Error::msg)
        })
        .map(|oci_ref| oci_ref.digest().is_some())
        .ok()
        .unwrap_or_default()
}

/// Given a refspec, infer its type and return it.
pub(crate) fn refspec_classify(refspec: &str) -> crate::ffi::RefspecType {
    if OstreeImageReference::try_from(refspec).is_ok() {
        crate::ffi::RefspecType::Container
    } else if ostree::validate_checksum_string(refspec).is_ok() {
        crate::ffi::RefspecType::Checksum
    } else {
        // fall back to Ostree if we cannot infer type
        crate::ffi::RefspecType::Ostree
    }
}

/// Perform reversible filesystem transformations necessary before we execute scripts.
pub(crate) struct FilesystemScriptPrep {
    rootfs: Dir,
    enabled: bool,
}

pub(crate) fn prepare_filesystem_script_prep(rootfs: i32) -> CxxResult<Box<FilesystemScriptPrep>> {
    let rootfs = unsafe { ffi_dirfd(rootfs)? };
    Ok(FilesystemScriptPrep::new(rootfs)?)
}

/// Using the Rust log infrastructure, print the treefile.
pub(crate) fn log_treefile(tf: &crate::treefile::Treefile) {
    tracing::debug!("Using treefile:\n{}", tf.get_json_string());
}

impl FilesystemScriptPrep {
    /// Filesystem paths that we rename out of the way if present
    const OPTIONAL_PATHS: &'static [&'static str] = &[SSS_CACHE_PATH];
    const REPLACE_OPTIONAL_PATHS: &'static [(&'static str, &'static [u8])] = &[
        (GROUPADD_PATH, GROUPADD_WRAPPER),
        (SYSTEMCTL_PATH, SYSTEMCTL_WRAPPER),
        (USERADD_PATH, USERADD_WRAPPER),
        (USERMOD_PATH, USERMOD_WRAPPER),
    ];

    // Separate const as is used only to wrap kernel-install if !is_ostree_layout()
    const REPLACE_KERNEL_PATHS: &'static [(&'static str, &'static [u8])] =
        &[(KERNEL_INSTALL_PATH, KERNEL_INSTALL_WRAPPER)];

    fn saved_name(name: &str) -> String {
        format!("{}.rpmostreesave", name)
    }

    #[context("Preparing filesystem for scripts")]
    pub(crate) fn new(rootfs: Dir) -> Result<Box<Self>> {
        for &path in Self::OPTIONAL_PATHS {
            if rootfs.try_exists(path)? {
                rootfs.rename(path, &rootfs, Self::saved_name(path))?;
            }
        }
        for &(path, contents) in Self::REPLACE_OPTIONAL_PATHS {
            let mode = Permissions::from_mode(0o755);
            let saved = &Self::saved_name(path);
            if rootfs.try_exists(path)? {
                rootfs.rename(path, &rootfs, saved)?;
                rootfs.atomic_write_with_perms(path, contents, mode)?;
            }
        }
        if !is_ostree_layout(&rootfs)? {
            for &(path, contents) in Self::REPLACE_KERNEL_PATHS {
                let mode = Permissions::from_mode(0o755);
                let saved = &Self::saved_name(path);
                if rootfs.try_exists(path)? {
                    rootfs.rename(path, &rootfs, saved)?;
                    rootfs.atomic_write_with_perms(path, contents, mode)?;
                }
            }
        }
        Ok(Box::new(Self {
            rootfs,
            enabled: true,
        }))
    }

    /// Undo the filesystem changes.
    #[context("Undoing prep filesystem for scripts")]
    pub(crate) fn undo(&mut self) -> CxxResult<()> {
        if !self.enabled {
            return Ok(());
        }
        for &path in Self::OPTIONAL_PATHS
            .iter()
            .chain(Self::REPLACE_OPTIONAL_PATHS.iter().map(|x| &x.0))
            .chain(Self::REPLACE_KERNEL_PATHS.iter().map(|x| &x.0))
        {
            let saved = &Self::saved_name(path);
            if self.rootfs.try_exists(saved)? {
                self.rootfs.rename(saved, &self.rootfs, path)?;
            }
        }
        self.enabled = false;
        Ok(())
    }
}

impl Drop for FilesystemScriptPrep {
    fn drop(&mut self) {
        let _ = self.undo();
    }
}

/// Some Fedora/RHEL kernels ship .hmac files with absolute paths inside,
/// which breaks when we relocate them into ostree/.  This function
/// changes them to be relative.
///
/// This is fixed in:
/// https://gitlab.com/cki-project/kernel-ark/-/merge_requests/1725
/// Until this lands everywhere we care about, we need this hack.
fn verify_kernel_hmac_impl(moddir: &Dir) -> Result<()> {
    // FIXME: in 2023
    // This method is intentionally a misnomer because it should eventually
    // be changed to the "sanity check" (below). It currently patches absolute
    // paths to give kernel package maintainers time to update their .spec files.

    const SEPARATOR: &str = "  ";

    let hmac_path = ".vmlinuz.hmac";

    let hmac_contents = if let Some(mut f) = moddir.open_optional(hmac_path)?.map(BufReader::new) {
        let mut s = String::new();
        f.read_to_string(&mut s)?;
        s
    } else {
        return Ok(());
    };

    // If the path is already relative, we're good.
    if !hmac_contents.contains('/') {
        return Ok(());
    }

    let (hmac, path) = hmac_contents
        .split_once(SEPARATOR)
        .ok_or_else(|| anyhow!("Missing path in .vmlinuz.hmac: {}", hmac_contents))?;
    let path = Utf8Path::new(path);

    let file_name = path
        .file_name()
        .ok_or_else(|| anyhow!("Missing filename in .vmlinuz.hmac: {}", hmac_contents))?;

    let new_contents = [hmac, SEPARATOR, file_name].concat();
    // sanity check
    if new_contents.contains('/') {
        return Err(anyhow!("Unexpected '/' in .vmlinuz.hmac: {}", new_contents));
    }

    let perms = Permissions::from_mode(0o644);
    moddir.atomic_write_with_perms(hmac_path, new_contents, perms)?;

    Ok(())
}

pub(crate) fn verify_kernel_hmac(rootfs: i32, moddir: &str) -> CxxResult<()> {
    let d = unsafe { &ffi_dirfd(rootfs)? };
    let moddir = d.open_dir(moddir)?;
    verify_kernel_hmac_impl(&moddir).map_err(Into::into)
}

/// Check if the commit has a serialized selinux policy sha256 that matches
/// the target policy's sha256.
pub(crate) fn commit_has_matching_sepolicy(
    commit: &crate::FFIGVariant,
    policy: &crate::FFIOstreeSePolicy,
) -> CxxResult<bool> {
    let commit = commit.glib_reborrow();
    let policy = policy.glib_reborrow();

    let sepolicy_csum = policy
        .csum()
        .ok_or_else(|| anyhow!("SELinux enabled, but no policy found"))?;

    let commitmeta = commit.child_value(0);
    let commitmeta = &glib::VariantDict::new(Some(&commitmeta));
    let key = "rpmostree.sepolicy";
    let v = commitmeta
        .lookup::<String>(key)
        .map_err(anyhow::Error::msg)?
        .ok_or_else(|| anyhow!("Missing metadata key {}", key))?;
    Ok(sepolicy_csum.as_str() == v.as_str())
}

/// Extract the rpm header as a GVariant of type ay (byte array)
pub(crate) fn get_header_variant(
    repo: &crate::FFIOstreeRepo,
    cachebranch: &str,
) -> CxxResult<*mut crate::FFIGVariant> {
    let repo = repo.glib_reborrow();

    let cached_rev = repo.require_rev(cachebranch)?;
    let cached_rev = cached_rev.as_str();
    let commit = repo.load_commit(cached_rev)?.0;
    let commitmeta = commit.child_value(0);
    let commitmeta = &glib::VariantDict::new(Some(&commitmeta));
    let key = "rpmostree.metadata";
    let r = commitmeta
        .lookup_value(key, Some(&*Vec::<u8>::static_variant_type()))
        .ok_or_else(|| anyhow!("Missing metadata key {}", key))
        .with_context(|| {
            let nevra = crate::rpmutils::cache_branch_to_nevra(cachebranch);
            format!("In commit {cached_rev} for {nevra}")
        })?;
    let p = r.as_ptr();
    std::mem::forget(r);
    Ok(p as *mut _)
}

pub(crate) fn stage_container_rpms(rpms: Vec<String>) -> CxxResult<Vec<String>> {
    let rpms: Result<Vec<File>> = rpms
        .into_iter()
        .map(|path| File::open(path).map_err(Into::into))
        .collect();
    stage_container_rpm_files(rpms?)
}

pub(crate) fn stage_container_rpm_raw_fds(fds: Vec<i32>) -> CxxResult<Vec<String>> {
    stage_container_rpm_files(
        fds.into_iter()
            .map(|fd| unsafe { File::from_raw_fd(fd) })
            .collect(),
    )
}

fn stage_container_rpm_files(rpms: Vec<File>) -> CxxResult<Vec<String>> {
    let mut r = Vec::new();
    let mut sack = dnf_sack_new();
    // XXX: This is really ugly: libdnf enforces that the filename ends in `.rpm`. So we use this
    // tempdir to hold symlinks to the fdpaths to fool it. Yuck. And we can't use cap_tempfile here
    // because the symlinks we create lead outside.
    let d = tempfile::tempdir()?;
    for mut rpm in rpms.into_iter() {
        let fdpath = format!("/proc/self/fd/{}", rpm.as_raw_fd());
        let symlink = format!("{}/{}.rpm", d.path().to_str().unwrap(), rpm.as_raw_fd());
        std::os::unix::fs::symlink(&fdpath, &symlink)?;
        let mut pkg = sack.pin_mut().add_cmdline_package(symlink)?;
        let chksum = crate::ffi::get_repodata_chksum_repr(&mut pkg.pin_mut().get_ref())?;
        let (alg, digest) = chksum
            .split_once(':')
            .ok_or_else(|| anyhow!("Missing ':' in chksum repr: {}", &chksum))?;
        if alg != "sha256" {
            return Err(anyhow!("expected sha256 hash, got {}", alg).into());
        }
        let staged_fn = format!("{}.rpm", digest);
        let run = cap_std::fs::Dir::open_ambient_dir("/run", cap_std::ambient_authority())?;
        run.create_dir_all(RPMOSTREE_CORE_STAGED_RPMS_DIR)?;
        let staged_rpms_dir = run.open_dir(RPMOSTREE_CORE_STAGED_RPMS_DIR)?;
        staged_rpms_dir.atomic_replace_with(&staged_fn, |f| -> std::io::Result<_> {
            std::io::copy(&mut rpm, f)
        })?;
        r.push(format!("{}:{}", digest, pkg.pin_mut().get_nevra()));
    }
    Ok(r)
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::capstdext::dirbuilder_from_mode;
    use anyhow::Result;
    use cap_std_ext::cap_tempfile;

    #[test]
    fn etcguard() -> Result<()> {
        let d = cap_tempfile::tempdir(cap_std::ambient_authority())?;
        let g = super::prepare_tempetc_guard(d.as_raw_fd())?;
        g.undo()?;
        let mut db = dirbuilder_from_mode(0o755);
        db.recursive(true);
        d.ensure_dir_with("usr/etc/foo", &db)?;
        assert!(!d.try_exists("etc/foo")?);
        let g = super::prepare_tempetc_guard(d.as_raw_fd())?;
        assert!(d.try_exists("etc/foo")?);
        g.undo()?;
        assert!(!d.try_exists("etc")?);
        assert!(d.try_exists("usr/etc/foo")?);
        Ok(())
    }

    #[test]
    fn rootfs() -> Result<()> {
        let d = cap_tempfile::tempdir(cap_std::ambient_authority())?;
        // The no-op case
        {
            let mut g = super::prepare_filesystem_script_prep(d.as_raw_fd())?;
            g.undo()?;
        }
        let mut db = dirbuilder_from_mode(0o755);
        let mode = Permissions::from_mode(0o755);
        db.recursive(true);
        d.ensure_dir_with("usr/bin", &db)?;
        d.ensure_dir_with("usr/sbin", &db)?;
        d.atomic_write_with_perms(super::SSS_CACHE_PATH, "sss binary", mode.clone())?;
        // Neutered sss_cache.
        {
            assert!(d.try_exists(super::SSS_CACHE_PATH)?);
            let mut g = super::prepare_filesystem_script_prep(d.as_raw_fd())?;
            assert!(!d.try_exists(super::SSS_CACHE_PATH)?);
            g.undo()?;
            assert!(d.try_exists(super::SSS_CACHE_PATH)?);
        }
        // Replaced systemctl.
        {
            let original_systemctl = "original systemctl";
            d.atomic_write_with_perms(super::SYSTEMCTL_PATH, original_systemctl, mode.clone())?;
            let contents = d.read_to_string(super::SYSTEMCTL_PATH)?;
            assert_eq!(contents, original_systemctl);
            let mut g = super::prepare_filesystem_script_prep(d.as_raw_fd())?;
            let contents = d.read_to_string(super::SYSTEMCTL_PATH)?;
            assert_eq!(contents.as_bytes(), super::SYSTEMCTL_WRAPPER);
            g.undo()?;
            let contents = d.read_to_string(super::SYSTEMCTL_PATH)?;
            assert_eq!(contents, original_systemctl);
        }
        // Replaced groupadd.
        {
            let original_groupadd = "original groupadd";
            d.atomic_write_with_perms(super::GROUPADD_PATH, original_groupadd, mode.clone())?;
            let contents = d.read_to_string(super::GROUPADD_PATH)?;
            assert_eq!(contents, original_groupadd);
            let mut g = super::prepare_filesystem_script_prep(d.as_raw_fd())?;
            let contents = d.read_to_string(super::GROUPADD_PATH)?;
            assert_eq!(contents.as_bytes(), super::GROUPADD_WRAPPER);
            g.undo()?;
            let contents = d.read_to_string(super::GROUPADD_PATH)?;
            assert_eq!(contents, original_groupadd);
        }
        // Replaced useradd.
        {
            let original_useradd = "original useradd";
            d.atomic_write_with_perms(super::USERADD_PATH, original_useradd, mode.clone())?;
            let contents = d.read_to_string(super::USERADD_PATH)?;
            assert_eq!(contents, original_useradd);
            let mut g = super::prepare_filesystem_script_prep(d.as_raw_fd())?;
            let contents = d.read_to_string(super::USERADD_PATH)?;
            assert_eq!(contents.as_bytes(), super::USERADD_WRAPPER);
            g.undo()?;
            let contents = d.read_to_string(super::USERADD_PATH)?;
            assert_eq!(contents, original_useradd);
        }
        // Replaced usermod.
        {
            let original_usermod = "original usermod";
            d.atomic_write_with_perms(super::USERMOD_PATH, original_usermod, mode.clone())?;
            let contents = d.read_to_string(super::USERMOD_PATH)?;
            assert_eq!(contents, original_usermod);
            let mut g = super::prepare_filesystem_script_prep(d.as_raw_fd())?;
            let contents = d.read_to_string(super::USERMOD_PATH)?;
            assert_eq!(contents.as_bytes(), super::USERMOD_WRAPPER);
            g.undo()?;
            let contents = d.read_to_string(super::USERMOD_PATH)?;
            assert_eq!(contents, original_usermod);
        }
        // Replaced kernel-install.
        {
            if !is_ostree_layout(&d)? {
                let original_kernel_install = "original kernel_install";
                d.atomic_write_with_perms(
                    super::KERNEL_INSTALL_PATH,
                    original_kernel_install,
                    mode,
                )?;
                let contents = d.read_to_string(super::KERNEL_INSTALL_PATH)?;
                assert_eq!(contents, original_kernel_install);
                let mut g = super::prepare_filesystem_script_prep(d.as_raw_fd())?;
                let contents = d.read_to_string(super::KERNEL_INSTALL_PATH)?;
                assert_eq!(contents.as_bytes(), super::KERNEL_INSTALL_WRAPPER);
                g.undo()?;
                let contents = d.read_to_string(super::KERNEL_INSTALL_PATH)?;
                assert_eq!(contents, original_kernel_install);
            }
        }
        Ok(())
    }

    #[test]
    fn test_refspecs() -> Result<()> {
        use super::is_container_image_reference;
        use super::refspec_classify;

        let refspec_type_checksum =
            "ee10f8e7ef638d78ba9a9596665067f58021624118875cc4079568da6c63efb0";
        assert!(!is_container_image_reference(refspec_type_checksum));
        assert_eq!(
            refspec_classify(refspec_type_checksum),
            crate::ffi::RefspecType::Checksum
        );

        let refspec_type_ostree_with_remote = "fedora:fedora/x86_64/coreos/testing-devel";
        assert!(!is_container_image_reference(
            refspec_type_ostree_with_remote
        ));
        assert_eq!(
            refspec_classify(refspec_type_ostree_with_remote),
            crate::ffi::RefspecType::Ostree
        );
        let refspec_type_ostree = "fedora/x86_64/coreos/foo-branch";
        assert!(!is_container_image_reference(refspec_type_ostree));
        assert_eq!(
            refspec_classify(refspec_type_ostree),
            crate::ffi::RefspecType::Ostree
        );

        const REFSPEC_TYPE_CONTAINER: &[&str] = &[
            "containers-storage:localhost/fcos:latest",
            "docker://quay.io/test-repository/os:version1",
            "registry:docker.io/test-repository/os:latest",
            "registry:customhostname.com:8080/test-repository/os:latest",
        ];

        for refspec in REFSPEC_TYPE_CONTAINER {
            let refspec = format!("ostree-unverified-image:{}", refspec);
            assert!(is_container_image_reference(&refspec));
            assert!(!is_container_image_digest_reference(&refspec));
            assert_eq!(
                refspec_classify(&refspec),
                crate::ffi::RefspecType::Container
            );
        }
        let refspec_type_container_digest = "docker://quay.io/test-repository/os@sha256:6006dca86c2dc549c123ff4f1dcbe60105fb05886531c93a3351ebe81dbe772f";
        let refspec = format!("ostree-unverified-image:{}", refspec_type_container_digest);
        assert!(is_container_image_reference(&refspec));
        assert!(is_container_image_digest_reference(&refspec));
        assert_eq!(
            refspec_classify(&refspec),
            crate::ffi::RefspecType::Container
        );

        Ok(())
    }

    #[test]
    fn verify_hmac() -> Result<()> {
        let d = cap_tempfile::tempdir(cap_std::ambient_authority())?;

        // No file is no-op
        verify_kernel_hmac_impl(&d).unwrap();

        // When the file is relative expect the function to be identity
        d.write(".vmlinuz.hmac", "abc123  a-relative-filename")?;
        verify_kernel_hmac_impl(&d).unwrap();
        assert_eq!(
            d.read_to_string(".vmlinuz.hmac")?,
            "abc123  a-relative-filename"
        );

        // Backwards compatability behavior
        d.write(".vmlinuz.hmac", "abc123  /an/absolute/filename.txt")?;
        verify_kernel_hmac_impl(&d).unwrap();
        assert_eq!(d.read_to_string(".vmlinuz.hmac")?, "abc123  filename.txt");

        // Sanity check compatability behavior
        d.write(".vmlinuz.hmac", "abc/123  /an/absolute/filename.txt")?;
        assert!(verify_kernel_hmac_impl(&d).is_err());

        Ok(())
    }
}
