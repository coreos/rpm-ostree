//! Helpers for interacting with containers-storage via forking podman.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::process::{Command, Stdio};

use anyhow::{Context, Result};
use camino::{Utf8Path, Utf8PathBuf};
use fn_error_context::context;

use crate::cmdutils::CommandRunExt;

pub(crate) struct PodmanMount {
    path: Utf8PathBuf,
    temp_cid: Option<String>,
    mounted: bool,
}

impl PodmanMount {
    /// Access the mount path.
    pub(crate) fn path(&self) -> &Utf8Path {
        &self.path
    }

    #[context("Unmounting container")]
    fn _impl_unmount(&mut self) -> Result<()> {
        if self.mounted {
            tracing::debug!("unmounting {}", self.path.as_str());
            self.mounted = false;
            Command::new("umount")
                .args(["-l", self.path.as_str()])
                .stdout(Stdio::null())
                .run()
                .context("umount")?;
            tracing::trace!("umount ok");
        }
        if let Some(cid) = self.temp_cid.take() {
            tracing::debug!("rm container {cid}");
            Command::new("podman")
                .args(["rm", cid.as_str()])
                .stdout(Stdio::null())
                .run()
                .context("podman rm")?;
            tracing::trace!("rm ok");
        }
        Ok(())
    }

    #[context("Mounting continer {container}")]
    fn _impl_mount(container: &str) -> Result<Utf8PathBuf> {
        let mut o = Command::new("podman")
            .args(["mount", container])
            .run_get_output()?;
        let mut s = String::new();
        o.read_to_string(&mut s)?;
        while s.ends_with('\n') {
            s.pop();
        }
        tracing::debug!("mounted container {container} at {s}");
        Ok(s.into())
    }

    #[allow(dead_code)]
    pub(crate) fn new_for_container(container: &str) -> Result<Self> {
        let path = Self::_impl_mount(container)?;
        Ok(Self {
            path,
            temp_cid: None,
            mounted: true,
        })
    }

    pub(crate) fn new_for_image(image: &str) -> Result<Self> {
        let mut o = Command::new("podman")
            .args(["create", image])
            .run_get_output()?;
        let mut s = String::new();
        o.read_to_string(&mut s)?;
        let cid = s.trim();
        let path = Self::_impl_mount(cid)?;
        tracing::debug!("created container {cid} from {image}");
        Ok(Self {
            path,
            temp_cid: Some(cid.to_owned()),
            mounted: true,
        })
    }

    pub(crate) fn unmount(mut self) -> Result<()> {
        self._impl_unmount()
    }
}

impl Drop for PodmanMount {
    fn drop(&mut self) {
        tracing::trace!("In drop, mounted={}", self.mounted);
        let _ = self._impl_unmount();
    }
}
