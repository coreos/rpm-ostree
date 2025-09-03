//! Helpers for interacting with containers-storage via forking podman.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::process::{Command, Stdio};

use anyhow::{Context, Result};
use camino::{Utf8Path, Utf8PathBuf};
use fn_error_context::context;

use bootc_internal_utils::CommandRunExt;

/// Ensure that we're in a new user+mountns, so that "buildah mount"
/// will work reliably.
/// https://github.com/containers/buildah/issues/5976
#[allow(dead_code)]
pub(crate) fn reexec_if_needed() -> Result<()> {
    if ostree_ext::container_utils::running_in_container() {
        crate::reexec::reexec_with_guardenv(
            "_RPMOSTREE_REEXEC_USERNS",
            &["unshare", "-U", "-m", "--map-root-user", "--keep-caps"],
        )
    } else {
        Ok(())
    }
}

/// We need to handle containers that only have podman, not buildah (like the -bootc ones)
/// as well as the inverse (like the buildah container).
#[derive(Debug, Copy, Clone)]
enum Backend {
    Podman,
    Buildah,
}

impl AsRef<str> for Backend {
    fn as_ref(&self) -> &'static str {
        match self {
            Backend::Podman => "podman",
            Backend::Buildah => "buildah",
        }
    }
}

pub(crate) struct Mount {
    backend: Backend,
    path: Utf8PathBuf,
    temp_cid: Option<String>,
    mounted: bool,
}

impl Mount {
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
                .run_capture_stderr()
                .context("umount")?;
            tracing::trace!("umount ok");
        }
        if let Some(cid) = self.temp_cid.take() {
            tracing::debug!("rm container {cid}");
            Command::new(self.backend.as_ref())
                .args(["rm", cid.as_str()])
                .stdout(Stdio::null())
                .run_capture_stderr()
                .context("podman rm")?;
            tracing::trace!("rm ok");
        }
        Ok(())
    }

    #[context("Mounting continer {container}")]
    fn _impl_mount(backend: Backend, container: &str) -> Result<Utf8PathBuf> {
        let mut o = Command::new(backend.as_ref())
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

    fn detect_backend() -> Result<Backend> {
        for case in [Backend::Podman, Backend::Buildah] {
            // Hardcode /usr/bin out of expedience
            let path = Utf8Path::new("/usr/bin").join(case.as_ref());
            if path.try_exists()? {
                tracing::debug!("Using backend {case:?}");
                return Ok(case);
            }
        }
        anyhow::bail!(
            "Failed to detect backend ({} or {})",
            Backend::Podman.as_ref(),
            Backend::Buildah.as_ref()
        );
    }

    #[allow(dead_code)]
    pub(crate) fn new_for_container(container: &str) -> Result<Self> {
        let backend = Self::detect_backend()?;
        let path = Self::_impl_mount(backend, container)?;
        Ok(Self {
            backend,
            path,
            temp_cid: None,
            mounted: true,
        })
    }

    pub(crate) fn new_for_image(image: &str) -> Result<Self> {
        let backend = Self::detect_backend()?;
        let mut o = match backend {
            Backend::Podman => Command::new(backend.as_ref())
                .args(["create", image])
                .run_get_output()?,
            Backend::Buildah => Command::new(backend.as_ref())
                .args(["from", image])
                .run_get_output()?,
        };
        let mut s = String::new();
        o.read_to_string(&mut s)?;
        let cid = s.trim();
        let path = Self::_impl_mount(backend, cid)?;
        tracing::debug!("created container {cid} from {image}");
        Ok(Self {
            backend,
            path,
            temp_cid: Some(cid.to_owned()),
            mounted: true,
        })
    }

    pub(crate) fn unmount(mut self) -> Result<()> {
        self._impl_unmount()
    }
}

impl Drop for Mount {
    fn drop(&mut self) {
        tracing::trace!("In drop, mounted={}", self.mounted);
        let _ = self._impl_unmount();
    }
}
