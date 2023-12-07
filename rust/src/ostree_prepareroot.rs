//! Logic related to parsing ostree-prepare-root.conf.
//!

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::io::BufReader;
use std::io::Read;

use anyhow::{Context, Result};
use camino::Utf8Path;
use cap_std::fs::Dir;
use cap_std_ext::dirext::CapStdExtDirExt;
use ostree_ext::glib;
use ostree_ext::keyfileext::KeyFileExt;

pub(crate) const CONF_PATH: &str = "ostree/prepare-root.conf";

pub(crate) fn load_config(rootfs: &Dir) -> Result<Option<glib::KeyFile>> {
    let kf = glib::KeyFile::new();
    for path in ["etc", "usr/lib"].into_iter().map(Utf8Path::new) {
        let path = &path.join(CONF_PATH);
        if let Some(fd) = rootfs
            .open_optional(path)
            .with_context(|| format!("Opening {path}"))?
        {
            let mut fd = BufReader::new(fd);
            let mut buf = String::new();
            fd.read_to_string(&mut buf)
                .with_context(|| format!("Reading {path}"))?;
            kf.load_from_data(&buf, glib::KeyFileFlags::NONE)
                .with_context(|| format!("Parsing {path}"))?;
            tracing::debug!("Loaded {path}");
            return Ok(Some(kf));
        }
    }
    tracing::debug!("No {CONF_PATH} found");
    Ok(None)
}

/// Query whether the target root has the `root.transient` key
/// which sets up a transient overlayfs.
pub(crate) fn transient_root_enabled(rootfs: &Dir) -> Result<bool> {
    if let Some(config) = load_config(rootfs)? {
        Ok(config
            .optional_bool("root", "transient")?
            .unwrap_or_default())
    } else {
        Ok(false)
    }
}
