//! Create Linux containers using bubblewrap AKA `/usr/bin/bwrap`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cmdutils::CommandRunExt;
use crate::cxxrsutil::*;
use crate::ffi::BubblewrapMutability;
use crate::normalization;
use anyhow::{Context, Result};
use camino::Utf8Path;
use camino::Utf8PathBuf;
use cap_std::fs::Dir;
use cap_std_ext::prelude::{CapStdExtCommandExt, CapStdExtDirExt};
use fn_error_context::context;
use ostree_ext::{gio, glib};
use std::num::NonZeroUsize;
use std::os::unix::io::AsRawFd;
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::Command;

// Links in the rootfs to /usr
static USR_LINKS: &[&str] = &["lib", "lib32", "lib64", "bin", "sbin"];
// This is similar to what systemd does, except we drop /usr/local, since scripts shouldn't see it.
static PATH_VAR: &str = "PATH=/usr/sbin:/usr/bin";
/// Explicitly added capabilities.  See the bits that call --cap-drop below for more
/// information.
static ADDED_CAPABILITIES: &[&str] = &[
    "cap_chown",
    "cap_dac_override",
    "cap_fowner",
    "cap_fsetid",
    "cap_kill",
    "cap_setgid",
    "cap_setuid",
    "cap_setpcap",
    "cap_sys_chroot",
    "cap_setfcap",
];

/// Filesystems explicitly mounted readonly; these were cargo culted from
/// docker/podman I think at some point.
static RO_BINDS: &[&str] = &[
    "/sys/block",
    "/sys/bus",
    "/sys/class",
    "/sys/dev",
    "/sys/devices",
];

pub(crate) struct Bubblewrap {
    pub(crate) rootfs_fd: Dir,

    executed: bool,
    argv: Vec<String>,
    child_argv0: Option<NonZeroUsize>,
    launcher: gio::SubprocessLauncher, // 🚀

    rofiles_mounts: Vec<RoFilesMount>,
}

// nspawn by default doesn't give us CAP_NET_ADMIN; see
// https://pagure.io/releng/issue/6602#comment-71214
// https://pagure.io/koji/pull-request/344#comment-21060
// Theoretically we should do capable(CAP_NET_ADMIN)
// but that's a lot of ugly code, and the only known
// place we hit this right now is nspawn.  Plus
// we want to use userns down the line anyways where
// we'll regain CAP_NET_ADMIN.
fn running_in_nspawn() -> bool {
    std::env::var_os("container").as_deref() == Some(std::ffi::OsStr::new("systemd-nspawn"))
}

/// A wrapper for rofiles-fuse from ostree.  This protects the underlying
/// hardlinked files from mutation.  The mount point is a temporary
/// directory.
struct RoFilesMount {
    /// Our inner tempdir; this is only an Option<T> so we can take ownership
    /// of it in drop().
    tempdir: Option<tempfile::TempDir>,
}

impl RoFilesMount {
    /// Create a new rofiles-fuse mount point
    fn new(rootfs: &Dir, path: &str) -> Result<Self> {
        let path = path.trim_start_matches('/');
        let tempdir = tempfile::Builder::new()
            .prefix("rpmostree-rofiles-fuse")
            .tempdir()?;
        let mut c = std::process::Command::new("rofiles-fuse");
        c.arg("--copyup")
            .arg(path)
            .arg(tempdir.path())
            .cwd_dir(rootfs.try_clone()?);
        unsafe {
            c.pre_exec(|| {
                rustix::process::set_parent_process_death_signal(Some(
                    rustix::process::Signal::Term,
                ))
                .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e))
            });
        }
        c.run()?;
        Ok(Self {
            tempdir: Some(tempdir),
        })
    }

    /// Return the mount point path
    fn path(&self) -> &Path {
        // Safety: We only use the Option<T> here around drop handling, it should always
        // be `Some` until `drop()` is called.
        self.tempdir.as_ref().unwrap().path()
    }
}

fn get_fusermount_path() -> Result<Utf8PathBuf> {
    let path = std::env::var("PATH").expect("PATH set");
    let fusermount_binaries = ["fusermount", "fusermount3"];
    for elt in path.split(':').map(Utf8Path::new) {
        for bin in fusermount_binaries {
            let target = elt.join(bin);
            if target.try_exists()? {
                return Ok(target);
            }
        }
    }
    anyhow::bail!("No fusermount path found")
}

impl Drop for RoFilesMount {
    fn drop(&mut self) {
        let tempdir = if let Some(d) = self.tempdir.take() {
            d
        } else {
            return;
        };
        // We need to unmount before letting the tempdir cleanup run.
        let success = Command::new(get_fusermount_path().unwrap())
            .arg("-u")
            .arg(tempdir.path())
            .status()
            .map_err(anyhow::Error::new)
            .and_then(|status| -> Result<()> {
                if !status.success() {
                    Err(anyhow::anyhow!("{}", status))
                } else {
                    Ok(())
                }
            })
            .err()
            .map(|e| {
                eprintln!("{}", e);
            })
            .is_none();
        if !success {
            // If fusermount fails, then we cannot remove it; just leak it.
            let _ = tempdir.into_path();
        }
    }
}

/// Helper wrapper that waits for a child and checks its exit status.
/// Further if the wait is cancelled then the child is force killed.
fn child_wait_check(
    child: gio::Subprocess,
    cancellable: Option<&gio::Cancellable>,
) -> std::result::Result<(), glib::Error> {
    match child.wait_check(cancellable) {
        Ok(_) => Ok(()),
        Err(e) => {
            child.force_exit();
            Err(e)
        }
    }
}

impl Bubblewrap {
    /// Create a new Bubblewrap instance
    pub(crate) fn new(rootfs_fd: &Dir) -> Result<Self> {
        let rootfs_fd = rootfs_fd.try_clone()?;

        let lang = std::env::var_os("LANG");
        let lang = lang.as_ref().and_then(|s| s.to_str()).unwrap_or("C");
        let lang_var = format!("LANG={lang}");
        let lang_var = Path::new(&lang_var);

        let launcher = gio::SubprocessLauncher::new(gio::SubprocessFlags::NONE);
        let child_rootfs_fd = std::sync::Arc::new(rootfs_fd.try_clone()?);
        launcher.set_child_setup(move || {
            rustix::process::fchdir(&*child_rootfs_fd).expect("fchdir");
        });

        let path_var = Path::new(PATH_VAR);
        launcher.set_environ(&[lang_var, path_var]);

        if let Some(source_date_epoch) = normalization::source_date_epoch_raw() {
            launcher.setenv("SOURCE_DATE_EPOCH", source_date_epoch, true);
        }

        // ⚠⚠⚠ If you change this, also update scripts/bwrap-script-shell.sh ⚠⚠⚠
        let mut argv = vec![
            "bwrap",
            "--dev",
            "/dev",
            "--proc",
            "/proc",
            "--dir",
            "/run",
            "--dir",
            "/tmp",
            "--chdir",
            "/",
            "--die-with-parent", /* Since 0.1.8 */
            /* Here we do all namespaces except the user one.
             * Down the line we want to do a userns too I think,
             * but it may need some mapping work.
             */
            "--unshare-pid",
            "--unshare-uts",
            "--unshare-ipc",
            "--unshare-cgroup-try",
        ];

        for d in RO_BINDS {
            argv.push("--ro-bind");
            argv.push(d);
            argv.push(d);
        }

        if !running_in_nspawn() {
            argv.push("--unshare-net");
        }

        /* Capabilities; this is a subset of the Docker (1.13 at least) default.
         * Specifically we strip out in addition to that:
         *
         * "cap_net_raw" (no use for this in %post, and major source of security vulnerabilities)
         * "cap_mknod" (%post should not be making devices, it wouldn't be persistent anyways)
         * "cap_audit_write" (we shouldn't be auditing anything from here)
         * "cap_net_bind_service" (nothing should be doing IP networking at all)
         *
         * But crucially we're dropping a lot of other capabilities like
         * "cap_sys_admin", "cap_sys_module", etc that Docker also drops by default.
         * We don't want RPM scripts to be doing any of that. Instead, do it from
         * systemd unit files.
         *
         * Also this way we drop out any new capabilities that appear.
         */
        if rustix::process::getuid().as_raw() == 0 {
            argv.extend(&["--cap-drop", "ALL"]);
            for cap in ADDED_CAPABILITIES {
                argv.push("--cap-add");
                argv.push(cap);
            }
        }

        let mut argv: Vec<_> = argv.into_iter().map(|s| s.to_string()).collect();

        for &name in USR_LINKS.iter() {
            if let Some(stbuf) = rootfs_fd.symlink_metadata_optional(name)? {
                if !stbuf.is_symlink() {
                    continue;
                }

                argv.push("--symlink".to_string());
                argv.push(format!("usr/{name}"));
                argv.push(format!("/{name}"));
            }
        }

        Ok(Self {
            rootfs_fd,
            executed: false,
            argv,
            launcher,
            child_argv0: None,
            rofiles_mounts: Vec::new(),
        })
    }

    /// Create a new bwrap instance with the provided level of mutability.
    pub(crate) fn new_with_mutability(
        rootfs_fd: &Dir,
        mutability: BubblewrapMutability,
    ) -> Result<Self> {
        let mut ret = Self::new(rootfs_fd)?;
        match mutability {
            BubblewrapMutability::Immutable => {
                ret.bind_read("usr", "/usr");
                ret.bind_read("etc", "/etc");
            }
            BubblewrapMutability::RoFiles => {
                ret.setup_rofiles("/usr")?;
                ret.setup_rofiles("/etc")?;
            }
            BubblewrapMutability::MutateFreely => {
                ret.bind_readwrite("usr", "/usr");
                ret.bind_readwrite("etc", "/etc");
            }
            o => {
                panic!("Invalid BubblewrapMutability: {:?}", o);
            }
        }
        Ok(ret)
    }

    fn setup_rofiles(&mut self, path: &str) -> Result<()> {
        let mnt = RoFilesMount::new(&self.rootfs_fd, path)?;
        let tmpdir_path = mnt.path().to_str().expect("tempdir str");
        self.bind_readwrite(tmpdir_path, path);
        self.rofiles_mounts.push(mnt);
        Ok(())
    }

    /// Access the underlying rootfs file descriptor (should only be used by C)
    pub(crate) fn get_rootfs_fd(&self) -> i32 {
        self.rootfs_fd.as_raw_fd()
    }

    /// Add an argument to the bubblewrap invocation.
    pub(crate) fn append_bwrap_arg(&mut self, arg: &str) {
        self.append_bwrap_argv(&[arg])
    }

    /// Add multiple arguments to the bubblewrap invocation.
    pub(crate) fn append_bwrap_argv(&mut self, args: &[&str]) {
        self.argv.extend(args.iter().map(|s| s.to_string()));
    }

    /// Add an argument for the child process.
    pub(crate) fn append_child_arg(&mut self, arg: &str) {
        self.append_child_argv([arg])
    }

    /// Add multiple arguments for the child process.
    pub(crate) fn append_child_argv<'a>(&mut self, args: impl IntoIterator<Item = &'a str>) {
        // Record the binary name for later error messages
        if self.child_argv0.is_none() {
            self.child_argv0 = Some(self.argv.len().try_into().expect("args"));
        }
        self.argv.extend(args.into_iter().map(|s| s.to_string()));
    }

    /// Set an environment variable
    pub(crate) fn setenv(&mut self, k: &str, v: &str) {
        self.launcher.setenv(k, v, true);
    }

    /// Take a file descriptor
    pub(crate) fn take_fd(&mut self, source_fd: i32, target_fd: i32) {
        self.launcher.take_fd(source_fd, target_fd);
    }

    /// Inherit stdin
    pub(crate) fn set_inherit_stdin(&mut self) {
        self.launcher.set_flags(gio::SubprocessFlags::STDIN_INHERIT);
    }

    /// Take a file descriptor for stdin
    pub(crate) fn take_stdin_fd(&mut self, source_fd: i32) {
        self.launcher.take_stdin_fd(source_fd);
    }

    /// Take a file descriptor for stdout
    pub(crate) fn take_stdout_fd(&mut self, source_fd: i32) {
        self.launcher.take_stdout_fd(source_fd);
    }

    /// Take a file descriptor for stderr
    pub(crate) fn take_stderr_fd(&mut self, source_fd: i32) {
        self.launcher.take_stderr_fd(source_fd);
    }

    /// Take a file descriptor for stderr
    pub(crate) fn take_stdout_and_stderr_fd(&mut self, source_fd: i32) {
        self.launcher.take_stdout_fd(source_fd);
        self.launcher.set_flags(gio::SubprocessFlags::STDERR_MERGE);
    }

    /// Bind source to destination in the container (readonly)
    pub(crate) fn bind_read(&mut self, src: &str, dest: &str) {
        self.append_bwrap_argv(&["--ro-bind", src, dest]);
    }

    /// Bind source to destination in the container (read-write)
    pub(crate) fn bind_readwrite(&mut self, src: &str, dest: &str) {
        self.append_bwrap_argv(&["--bind", src, dest]);
    }

    /// Set /var to be read-only, but with a transient writable /var/tmp
    /// and compat symlinks for scripts.
    pub(crate) fn setup_compat_var(&mut self) -> CxxResult<()> {
        use crate::composepost::COMPAT_VARLIB_SYMLINKS;

        for entry in COMPAT_VARLIB_SYMLINKS {
            let varlib_path = format!("var/lib/{}", &entry);
            match self.rootfs_fd.symlink_metadata(&varlib_path) {
                Ok(_) => {}
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                    let target = format!("../../usr/lib/{}", &entry);
                    self.rootfs_fd
                        .symlink(&target, &varlib_path)
                        .with_context(|| {
                            format!("Creating compatibility symlink at /var/lib/{}", &entry)
                        })?;
                }
                Err(error) => return Err(error.into()),
            };
        }

        self.bind_read("./var", "/var");
        self.append_bwrap_argv(&["--tmpfs", "/var/tmp"]);

        Ok(())
    }

    /// Launch the process, returning a handle as well as a description for argv0
    fn spawn(&mut self) -> Result<(gio::Subprocess, String)> {
        assert!(!self.executed);
        self.executed = true;

        let child_argv0_i: usize = self.child_argv0.expect("child argument").into();
        let child_argv0 = format!("bwrap({})", self.argv[child_argv0_i].as_str());
        let argv: Vec<_> = self.argv.iter().map(|s| s.as_ref()).collect();
        let child = self.launcher.spawn(&argv)?;
        Ok((child, child_argv0))
    }

    /// Execute the container, capturing stdout.
    pub(crate) fn run_captured(
        &mut self,
        cancellable: Option<&gio::Cancellable>,
    ) -> Result<glib::Bytes> {
        self.launcher.set_flags(gio::SubprocessFlags::STDOUT_PIPE);
        let (child, argv0) = self.spawn()?;
        let (stdout, stderr) = child.communicate(None, cancellable)?;
        // we never pipe just stderr, so we don't expect it to be captured
        assert!(stderr.is_none());
        let stdout = stdout.expect("stdout");

        child_wait_check(child, cancellable).context(argv0)?;

        Ok(stdout)
    }

    /// Execute the container.  This method uses the normal gtk-rs `Option<T>` for the cancellable.
    fn run_inner(&mut self, cancellable: Option<&gio::Cancellable>) -> Result<()> {
        let (child, argv0) = self.spawn()?;
        child_wait_check(child, cancellable).context(argv0)?;
        Ok(())
    }

    /// Execute the instance; requires a cancellable for C++.
    pub(crate) fn run(&mut self, cancellable: &crate::FFIGCancellable) -> CxxResult<()> {
        let cancellable = &cancellable.glib_reborrow();
        self.run_inner(Some(cancellable))?;
        Ok(())
    }
}

#[context("Creating bwrap instance")]
/// Create a new Bubblewrap instance
pub(crate) fn bubblewrap_new(rootfs_fd: i32) -> CxxResult<Box<Bubblewrap>> {
    let rootfs_fd = unsafe { crate::ffiutil::ffi_dirfd(rootfs_fd)? };
    Ok(Box::new(Bubblewrap::new(&rootfs_fd)?))
}

#[context("Creating bwrap instance")]
/// Create a new Bubblewrap instance with provided mutability
pub(crate) fn bubblewrap_new_with_mutability(
    rootfs_fd: i32,
    mutability: crate::ffi::BubblewrapMutability,
) -> Result<Box<Bubblewrap>> {
    let rootfs_fd = unsafe { crate::ffiutil::ffi_dirfd(rootfs_fd)? };
    Ok(Box::new(Bubblewrap::new_with_mutability(
        &rootfs_fd, mutability,
    )?))
}

/// Synchronously run bubblewrap, allowing mutability, and optionally capturing stdout.
pub(crate) fn bubblewrap_run_sync(
    rootfs_dfd: i32,
    args: &Vec<String>,
    capture_stdout: bool,
    mutability: BubblewrapMutability,
) -> CxxResult<Vec<u8>> {
    let rootfs_dfd = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
    let tempetc = crate::core::prepare_tempetc_guard(rootfs_dfd.as_raw_fd())?;
    let mut bwrap = Bubblewrap::new_with_mutability(rootfs_dfd, mutability)?;

    if mutability != BubblewrapMutability::MutateFreely {
        bwrap.bind_read("var", "/var")
    } else {
        bwrap.bind_readwrite("var", "/var")
    }

    bwrap.append_child_argv(args.iter().map(|s| s.as_str()));

    let cancellable = &gio::Cancellable::new();
    let cancellable = Some(cancellable);
    if capture_stdout {
        let buf = bwrap.run_captured(cancellable)?;
        tempetc.undo()?;
        Ok(buf.as_ref().to_vec())
    } else {
        bwrap.run_inner(cancellable)?;
        tempetc.undo()?;
        Ok(Vec::new())
    }
}

#[context("bwrap test failed, see <https://github.com/coreos/rpm-ostree/pull/429>")]
/// Validate that bubblewrap works at all.  This will flush out any incorrect
/// setups such being inside an outer container that disallows `CLONE_NEWUSER` etc.
pub(crate) fn bubblewrap_selftest() -> CxxResult<()> {
    let fd = &Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
    let mut bwrap = Bubblewrap::new_with_mutability(fd, BubblewrapMutability::Immutable)?;
    bwrap.append_child_argv(["true"]);
    let cancellable = &gio::Cancellable::new();
    let cancellable = Some(cancellable);
    bwrap.run_inner(cancellable)?;
    Ok(())
}
