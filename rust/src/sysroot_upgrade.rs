//! Rust portion of `rpmostree-sysroot-upgrader.cxx`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::io::Seek;
use std::process::Stdio;

use crate::capstdext::dirbuilder_from_mode;
use crate::cxxrsutil::*;
use crate::ffi::{output_message, ContainerImageState};
use crate::progress::progress_task;
use anyhow::{anyhow, Context, Result};
use camino::Utf8Path;
use cap_std::fs::Dir;
use cap_std_ext::prelude::CapStdExtCommandExt;
use fn_error_context::context;
use ostree::glib;
use ostree::prelude::*;
use ostree_container::store::{ImageImporter, PrepareResult};
use ostree_container::OstreeImageReference;
use ostree_ext::container as ostree_container;
use ostree_ext::container::store::{ImportProgress, ManifestLayerState};
use ostree_ext::ostree;
use std::pin::Pin;
use tokio::runtime::Handle;
use tokio::sync::mpsc::Receiver;

impl From<Box<ostree_container::store::LayeredImageState>> for crate::ffi::ContainerImageState {
    fn from(s: Box<ostree_container::store::LayeredImageState>) -> crate::ffi::ContainerImageState {
        let version = s
            .configuration
            .as_ref()
            .and_then(|c| c.config().as_ref())
            .and_then(|c| c.labels().as_ref())
            .and_then(|l| l.get("version"))
            .cloned()
            .unwrap_or_default();
        crate::ffi::ContainerImageState {
            base_commit: s.base_commit,
            merge_commit: s.merge_commit,
            is_layered: s.is_layered,
            image_digest: s.manifest_digest,
            version,
        }
    }
}

/// Return a two-tuple where the second element is a two-tuple too:
/// (number of layers already stored, (number of layers to fetch, size of layers to fetch))
fn layer_counts<'a>(layers: impl Iterator<Item = &'a ManifestLayerState>) -> (u32, (u32, u64)) {
    layers.fold(
        (0u32, (0u32, 0u64)),
        |(stored, (n_to_fetch, size_to_fetch)), v| {
            if v.commit.is_some() {
                (stored + 1, (n_to_fetch, size_to_fetch))
            } else {
                (stored, (n_to_fetch + 1, size_to_fetch + v.size()))
            }
        },
    )
}

async fn layer_progress_print(mut r: Receiver<ImportProgress>) {
    while let Some(v) = r.recv().await {
        let msg = ostree_ext::cli::layer_progress_format(&v);
        output_message(&msg);
    }
}

async fn pull_container_async(
    repo: &ostree::Repo,
    imgref: &OstreeImageReference,
) -> Result<ContainerImageState> {
    output_message(&format!("Pulling manifest: {}", &imgref));
    let mut imp = ImageImporter::new(repo, imgref, Default::default()).await?;
    let layer_progress = imp.request_progress();
    let prep = match imp.prepare().await? {
        PrepareResult::AlreadyPresent(r) => return Ok(r.into()),
        PrepareResult::Ready(r) => r,
    };
    let progress_printer =
        tokio::task::spawn(async move { layer_progress_print(layer_progress).await });
    let digest = prep.manifest_digest.clone();
    output_message(&format!("Importing: {} (digest: {})", &imgref, &digest));
    let ostree_layers = prep
        .ostree_layers
        .iter()
        .chain(std::iter::once(&prep.ostree_commit_layer));
    let (stored, (n_to_fetch, size_to_fetch)) = layer_counts(ostree_layers);
    if stored > 0 || n_to_fetch > 0 {
        let size = glib::format_size(size_to_fetch);
        output_message(&format!(
            "ostree chunk layers stored: {stored} needed: {n_to_fetch} ({size})"
        ));
    }
    let (stored, (n_to_fetch, size_to_fetch)) = layer_counts(prep.layers.iter());
    if stored > 0 || n_to_fetch > 0 {
        let size = glib::format_size(size_to_fetch);
        output_message(&format!(
            "custom layers stored: {stored} needed: {n_to_fetch} ({size})"
        ));
    }
    let import = imp.import(prep).await;
    let _ = progress_printer.await;
    // TODO log the discarded bits from import
    Ok(import?.into())
}

/// Import ostree commit in container image using ostree-rs-ext's API.
pub(crate) fn pull_container(
    mut repo: Pin<&mut crate::FFIOstreeRepo>,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
    imgref: &str,
) -> CxxResult<Box<ContainerImageState>> {
    let repo = &repo.gobj_wrap();
    let cancellable = cancellable.gobj_wrap();
    let imgref = &OstreeImageReference::try_from(imgref)?;

    let r = Handle::current().block_on(async {
        crate::utils::run_with_cancellable(
            async { pull_container_async(repo, imgref).await },
            &cancellable,
        )
        .await
    })?;
    Ok(Box::new(r))
}

pub(crate) fn layer_prune(
    repo: &ostree::Repo,
    cancellable: Option<&ostree::gio::Cancellable>,
) -> Result<()> {
    if let Some(c) = cancellable {
        c.set_error_if_cancelled()?;
    }
    tracing::debug!("pruning image layers");
    let n_pruned = ostree_ext::container::store::gc_image_layers(repo)?;
    systemd::journal::print(6, &format!("Pruned container image layers: {n_pruned}"));
    Ok(())
}

/// Run a prune of container image layers.
pub(crate) fn container_prune(
    repo: &crate::FFIOstreeRepo,
    cancellable: &crate::FFIGCancellable,
) -> CxxResult<()> {
    layer_prune(&repo.glib_reborrow(), Some(&cancellable.glib_reborrow())).map_err(Into::into)
}

/// C++ wrapper for querying image state; requires a pulled image
pub(crate) fn query_container_image_commit(
    repo: &crate::FFIOstreeRepo,
    imgcommit: &str,
) -> CxxResult<Box<crate::ffi::ContainerImageState>> {
    let repo = &repo.glib_reborrow();
    let state = ostree_container::store::query_image_commit(repo, imgcommit)?;
    Ok(Box::new(state.into()))
}

/// Remove a refspec, which can be either an ostree branch or a container image.
pub(crate) fn purge_refspec(repo: &crate::FFIOstreeRepo, imgref: &str) -> CxxResult<()> {
    let repo = &repo.glib_reborrow();
    tracing::debug!("Purging {imgref}");
    if let Ok(cref) = OstreeImageReference::try_from(imgref) {
        // It's a container, use the ostree-ext APIs to prune it.
        ostree_container::store::remove_image(repo, &cref.imgref)?;
        layer_prune(repo, None)?;
    } else if ostree::validate_checksum_string(imgref).is_ok() {
        // Nothing to do here
    } else {
        match ostree::parse_refspec(imgref) {
            Ok((remote, ostreeref)) => {
                repo.set_ref_immediate(
                    remote.as_ref().map(|s| s.as_str()),
                    &ostreeref,
                    None,
                    ostree::gio::NONE_CANCELLABLE,
                )?;
            }
            Err(e) => {
                // For historical reasons, we ignore errors here
                tracing::warn!("{e}");
            }
        }
    }
    Ok(())
}

#[context("Rewriting rpmdb")]
fn rewrite_rpmdb(rootfs_dfd: &Dir) -> Result<()> {
    use crate::composepost::RPMOSTREE_RPMDB_LOCATION;

    let mut dbfd = cap_tempfile::TempFile::new_anonymous(rootfs_dfd)?;

    // rpmdb wants an absolute file path for some reason
    let dbpath_arg = format!("--dbpath=/proc/self/cwd/{RPMOSTREE_RPMDB_LOCATION}");
    // Fork rpmdb from the *host* rootfs to read the rpmdb back into memory
    let r = std::process::Command::new("rpmdb")
        .args(&[dbpath_arg.as_str(), "--exportdb"])
        .cwd_dir(rootfs_dfd.try_clone()?)
        .stdout(Stdio::from(dbfd.try_clone()?))
        .status()
        .context("Spawning exportdb")?;
    if !r.success() {
        return Err(anyhow!("Failed to execute rpmdb --exportdb: {:?}", r));
    }

    // Clear out the db on disk
    if rootfs_dfd.exists(RPMOSTREE_RPMDB_LOCATION) {
        rootfs_dfd.remove_dir_all(RPMOSTREE_RPMDB_LOCATION)?;
    }
    let db = dirbuilder_from_mode(0o755);
    rootfs_dfd.create_dir_with(RPMOSTREE_RPMDB_LOCATION, &db)?;

    // Ensure the subsequent reader is starting from the beginning
    dbfd.seek(std::io::SeekFrom::Start(0))?;

    // And write the rpmdb back
    let r = std::process::Command::new("rpmdb")
        .args(&[dbpath_arg.as_str(), "--importdb"])
        .cwd_dir(rootfs_dfd.try_clone()?)
        .stdin(Stdio::from(dbfd))
        .status()
        .context("Spawning importdb")?;
    if !r.success() {
        return Err(anyhow!("Failed to execute rpmdb --importdb: {:?}", r));
    }

    Ok(())
}

/// Use the host's rpmdb to rewrite the RPM database in the host's preferred format.
/// This is used in our implementation of transitioning between RHEL8 (bdb) and RHEL9 (sqlite)
/// rpmdb formats.
fn handle_rpmdb_transition_impl(rootfs_dfd: &Dir) -> Result<()> {
    use crate::composepost::RPMOSTREE_RPMDB_LOCATION;

    // Since we're only shipping this code for RHEL8, this *should* return `bdb`
    let host_rpmdb_format = crate::ffi::util_get_rpmdb_format();
    let file_path = if let Some(n) = crate::core::filename_for_rpmdb_type(&host_rpmdb_format) {
        n
    } else {
        eprintln!("warning: Unhandled rpmdb format {host_rpmdb_format}; this may lead to unpredictable failures");
        return Ok(());
    };
    // We want to avoid the expense of export/import of the RPM database if we're *not* doing
    // a major version upgrade.  In other words, in the case where the rpmdb format is bdb
    // and the target root is bdb, then we have nothing to do here.
    if rootfs_dfd.try_exists(Utf8Path::new(RPMOSTREE_RPMDB_LOCATION).join(file_path))? {
        tracing::debug!("target rpmdb matches host {host_rpmdb_format}");
        return Ok(());
    }

    progress_task(
        &format!("Rewriting rpmdb back to host {host_rpmdb_format}"),
        || -> Result<_> { rewrite_rpmdb(rootfs_dfd) },
    )
}

#[context("Rewriting rpmdb back to host format")]
/// Rewrite the RPM database if we detect the target format is sqlite, and we're on bdb.
///
/// Since this patch is only destined for RHEL8 rpm-ostree, we assume that the running process can read/write bdb,
/// and just *read* sqlite.  Hence, we do a transition from sqlite back down to bdb.
///
/// Then, today while it wasn't intentional, things work out back in sqlite because
/// the `rewrite_rpmdb_for_target` code will kick in after all the package operations are done
/// and use the *target* system's `rpmdb --importdb` to convert back from bdb to sqlite.
pub(crate) fn handle_rpmdb_transition(rootfs_dfd: i32) -> Result<()> {
    let rootfs_dfd = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
    handle_rpmdb_transition_impl(rootfs_dfd)?;
    Ok(())
}
