//! Create Linux containers using bubblewrap AKA `/usr/bin/bwrap`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::CxxResult;
use anyhow::{anyhow, Context, Result};
use fn_error_context::context;
use std::os::unix::io::AsRawFd;
use std::rc::Rc;

pub(crate) struct Bubblewrap {
    pub(crate) rootfs_fd: Rc<openat::Dir>,
}

impl Bubblewrap {
    /// Create a new Bubblewrap instance
    pub(crate) fn new(rootfs_fd: &Rc<openat::Dir>) -> Self {
        Self {
            rootfs_fd: Rc::clone(rootfs_fd),
        }
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
}

#[context("Creating bwrap instance")]
/// Create a new Bubblewrap instance
pub(crate) fn bubblewrap_new(rootfs_fd: i32) -> CxxResult<Box<Bubblewrap>> {
    let fd = Rc::new(openat::Dir::open(format!("/proc/self/fd/{}", rootfs_fd))?);
    Ok(Box::new(Bubblewrap::new(&fd)))
}
