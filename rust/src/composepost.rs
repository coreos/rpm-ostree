//! Logic for post-processing a filesystem tree, server-side.
//!
//! This code runs server side to "postprocess" a filesystem tree (usually
//! containing mostly RPMs) in order to prepare it as an OSTree commit.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::bwrap::Bubblewrap;
use crate::capstdext::dirbuilder_from_mode;
use crate::cxxrsutil::*;
use crate::ffi::BubblewrapMutability;
use crate::ffiutil::ffi_dirfd;
use crate::normalization;
use crate::passwd::PasswdDB;
use crate::treefile::Treefile;
use crate::{bwrap, importer};
use anyhow::{anyhow, bail, format_err, Context, Result};
use camino::{Utf8Path, Utf8PathBuf};
use cap_std::fs::Dir;
use cap_std::fs_utf8::Dir as Utf8Dir;
use cap_std::io_lifetimes::AsFilelike;
use cap_std_ext::cap_std;
use cap_std_ext::cap_std::fs::{DirBuilderExt, MetadataExt, Permissions, PermissionsExt};
use cap_std_ext::dirext::CapStdExtDirExt;
use fn_error_context::context;
use gio::prelude::*;
use gio::FileType;
use ostree_ext::{gio, glib};
use rayon::prelude::*;
use std::borrow::Cow;
use std::collections::BTreeSet;
use std::fmt::Write as FmtWrite;
use std::io::{BufRead, BufReader, Seek, Write};
use std::os::unix::io::AsRawFd;
use std::os::unix::prelude::IntoRawFd;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::process::Stdio;

/// Directories that are moved out and symlinked from their `/var/lib/<entry>`
/// location to `/usr/lib/<entry>`.
pub(crate) static COMPAT_VARLIB_SYMLINKS: &[&str] = &["alternatives", "vagrant"];

const DEFAULT_DIRMODE: u32 = 0o755;

/// Symlinks to ensure home directories persist by default.
const OSTREE_HOME_SYMLINKS: &[(&str, &str)] = &[("var/roothome", "root"), ("var/home", "home")];

/* See rpmostree-core.h */
const RPMOSTREE_BASE_RPMDB: &str = "usr/lib/sysimage/rpm-ostree-base-db";
pub(crate) const RPMOSTREE_RPMDB_LOCATION: &str = "usr/share/rpm";
const RPMOSTREE_SYSIMAGE_RPMDB: &str = "usr/lib/sysimage/rpm";
pub(crate) const TRADITIONAL_RPMDB_LOCATION: &str = "var/lib/rpm";

const SD_LOCAL_FS_TARGET_REQUIRES: &str = "usr/lib/systemd/system/local-fs.target.requires";

#[context("Moving {}", name)]
fn dir_move_if_exists(src: &cap_std::fs::Dir, dest: &cap_std::fs::Dir, name: &str) -> Result<()> {
    if src.symlink_metadata(name).is_ok() {
        src.rename(name, dest, name)?;
    }
    Ok(())
}

/// Initialize an ostree-oriented root filesystem.
///
/// Now unfortunately today, we're not generating toplevel filesystem entries
/// because the `filesystem` package does it from Lua code, which we don't run.
/// (See rpmostree-core.cxx)
#[context("Initializing rootfs (base)")]
fn compose_init_rootfs_base(rootfs_dfd: &cap_std::fs::Dir, tmp_is_dir: bool) -> Result<()> {
    const TOPLEVEL_DIRS: &[&str] = &["dev", "proc", "run", "sys", "var", "sysroot"];

    let default_dirbuilder = &dirbuilder_from_mode(DEFAULT_DIRMODE);
    let default_dirmode = cap_std::fs::Permissions::from_mode(DEFAULT_DIRMODE);

    rootfs_dfd
        .set_permissions(".", default_dirmode)
        .context("Setting rootdir permissions")?;

    TOPLEVEL_DIRS.par_iter().try_for_each(|&d| {
        rootfs_dfd
            .ensure_dir_with(d, default_dirbuilder)
            .with_context(|| format!("Creating {d}"))
            .map(|_: bool| ())
    })?;

    if tmp_is_dir {
        let tmp_mode = 0o1777;
        rootfs_dfd
            .ensure_dir_with("tmp", &dirbuilder_from_mode(tmp_mode))
            .context("tmp")?;
        rootfs_dfd
            .set_permissions("tmp", cap_std::fs::Permissions::from_mode(tmp_mode))
            .context("Setting permissions for tmp")?;
    } else {
        rootfs_dfd.symlink("sysroot/tmp", "tmp")?;
    }

    OSTREE_HOME_SYMLINKS
        .par_iter()
        .try_for_each(|&(dest, src)| {
            rootfs_dfd
                .symlink(dest, src)
                .with_context(|| format!("Creating {src}"))
        })?;

    rootfs_dfd
        .symlink("sysroot/ostree", "ostree")
        .context("Symlinking ostree -> sysroot/ostree")?;

    Ok(())
}

/// Initialize a root filesystem set up for use with ostree's `root.transient` mode.
#[context("Initializing rootfs (base)")]
fn compose_init_rootfs_transient(rootfs_dfd: &cap_std::fs::Dir) -> Result<()> {
    // Enforce tmp-is-dir in this, because there's really no reason not to.
    compose_init_rootfs_base(rootfs_dfd, true)?;
    // Again we need to make these directories here because we don't run
    // the `filesystem` package's lua script.
    const EXTRA_TOPLEVEL_DIRS: &[&str] = &["opt", "media", "mnt", "usr/local"];

    let mut db = dirbuilder_from_mode(DEFAULT_DIRMODE);
    db.recursive(true);
    EXTRA_TOPLEVEL_DIRS.par_iter().try_for_each(|&d| {
        // We need to handle the case where these may have been created as a symlink
        // by tmpfiles.d snippets for example.
        if let Some(meta) = rootfs_dfd.symlink_metadata_optional(d)? {
            if !meta.is_dir() {
                rootfs_dfd.remove_file(d)?;
            }
        }
        rootfs_dfd
            .ensure_dir_with(d, &db)
            .with_context(|| format!("Creating {d}"))
            .map(|_: bool| ())
    })?;

    Ok(())
}

/// Initialize an ostree-oriented root filesystem.
///
/// This is hardcoded; in the future we may make more things configurable,
/// but the goal is for all state to be in `/etc` and `/var`.
#[context("Initializing rootfs")]
fn compose_init_rootfs_strict(
    rootfs_dfd: &cap_std::fs::Dir,
    tmp_is_dir: bool,
    opt_state_overlay: bool,
) -> Result<()> {
    println!("Initializing rootfs");

    compose_init_rootfs_base(rootfs_dfd, tmp_is_dir)?;

    const OPT_SYMLINK_LEGACY: &str = "var/opt";
    const OPT_SYMLINK_STATEOVERLAY: &str = "usr/lib/opt";
    let opt_symlink = if opt_state_overlay {
        OPT_SYMLINK_STATEOVERLAY
    } else {
        OPT_SYMLINK_LEGACY
    };

    // This is used in the case where we don't have a transient rootfs; redirect
    // these toplevel directories underneath /var.
    let ostree_strict_mode_symlinks: &[(&str, &str)] = &[
        (opt_symlink, "opt"),
        ("var/srv", "srv"),
        ("var/mnt", "mnt"),
        ("run/media", "media"),
    ];
    ostree_strict_mode_symlinks
        .par_iter()
        .try_for_each(|&(dest, src)| {
            rootfs_dfd
                .symlink(dest, src)
                .with_context(|| format!("Creating {src}"))
        })?;

    Ok(())
}

/// Prepare rootfs for commit.
///
/// In the default mode, we initialize a basic root filesystem in @target_root_dfd, then walk over the
/// root filesystem in @src_rootfs_fd and take the basic content (/usr, /boot, /var)
/// and cherry pick only specific bits of the rest of the toplevel like compatibility
/// symlinks (e.g. /lib64 -> /usr/lib64) if they exist.
///
/// However, if the rootfs is setup as transient, then we just copy everything.
#[context("Preparing rootfs for commit")]
pub fn compose_prepare_rootfs(
    src_rootfs_dfd: i32,
    target_rootfs_dfd: i32,
    treefile: &mut Treefile,
) -> CxxResult<()> {
    let src_rootfs_dfd = unsafe { &ffi_dirfd(src_rootfs_dfd)? };
    let target_rootfs_dfd = unsafe { &ffi_dirfd(target_rootfs_dfd)? };

    let tmp_is_dir = treefile.parsed.base.tmp_is_dir.unwrap_or_default();

    if crate::ostree_prepareroot::transient_root_enabled(src_rootfs_dfd)? {
        println!("Target has transient root enabled");
        // While sadly tmp-is-dir: false by default, we want to encourage
        // people to switch, so just error out if they're somehow configured
        // things for the newer transient root model but forgot to set `tmp-is-dir`.
        if !tmp_is_dir {
            return Err("Transient root conflicts with tmp-is-dir: false"
                .to_string()
                .into());
        }
        // We grab all the content from the source root by default on general principle,
        // but note this won't be very much right now because
        // we're not executing the `filesystem` package's lua script.
        for entry in src_rootfs_dfd.entries()? {
            let entry = entry?;
            let name = entry.file_name();
            src_rootfs_dfd
                .rename(&name, target_rootfs_dfd, &name)
                .with_context(|| format!("Moving {name:?}"))?;
        }
        compose_init_rootfs_transient(target_rootfs_dfd)?;
        return Ok(());
    }

    compose_init_rootfs_strict(
        target_rootfs_dfd,
        tmp_is_dir,
        treefile
            .parsed
            .base
            .opt_usrlocal_overlays
            .unwrap_or_default(),
    )?;

    println!("Moving /usr to target");
    src_rootfs_dfd.rename("usr", target_rootfs_dfd, "usr")?;
    /* The kernel may be in the source rootfs /boot; to handle that, we always
     * rename the source /boot to the target, and will handle everything after
     * that in the target root.
     */
    dir_move_if_exists(src_rootfs_dfd, target_rootfs_dfd, "boot")?;

    /* And grab /var - we'll convert to tmpfiles.d later */
    dir_move_if_exists(src_rootfs_dfd, target_rootfs_dfd, "var")?;

    const TOPLEVEL_LINKS: &[&str] = &["lib", "lib64", "lib32", "bin", "sbin"];
    println!("Copying toplevel compat symlinks");
    TOPLEVEL_LINKS
        .par_iter()
        .try_for_each(|&l| dir_move_if_exists(src_rootfs_dfd, target_rootfs_dfd, l))?;

    Ok(())
}

// rpm-ostree uses /home → /var/home by default as generated by our
// rootfs; we don't expect people to change this.  Let's be nice
// and also fixup the $HOME entries generated by `useradd` so
// that `~` shows up as expected in shells, etc.
//
// https://github.com/coreos/fedora-coreos-config/pull/18
// https://pagure.io/workstation-ostree-config/pull-request/121
// https://discussion.fedoraproject.org/t/adapting-user-home-in-etc-passwd/487/6
// https://github.com/justjanne/powerline-go/issues/94
#[context("Postprocessing useradd")]
fn postprocess_useradd(rootfs_dfd: &cap_std::fs::Dir) -> Result<()> {
    let path = Utf8Path::new("usr/etc/default/useradd");
    let perms = cap_std::fs::Permissions::from_mode(0o644);
    if let Some(f) = rootfs_dfd.open_optional(path).context("opening")? {
        rootfs_dfd
            .atomic_replace_with(&path, |bufw| -> Result<_> {
                bufw.get_mut().as_file_mut().set_permissions(perms)?;
                let f = BufReader::new(&f);
                for line in f.lines() {
                    let line = line?;
                    if !line.starts_with("HOME=") {
                        bufw.write_all(line.as_bytes())?;
                    } else {
                        bufw.write_all(b"HOME=/var/home")?;
                    }
                    bufw.write_all(b"\n")?;
                }
                Ok(())
            })
            .with_context(|| format!("Replacing {}", path))?;
    }
    Ok(())
}

fn is_overlay_whiteout(meta: &cap_std::fs::Metadata) -> bool {
    (meta.mode() & libc::S_IFMT) == libc::S_IFCHR && meta.rdev() == 0
}

/// Auto-synthesize embedded overlayfs whiteouts; for more information
/// see https://github.com/ostreedev/ostree/pull/2722/commits/0085494e350c72599fc5c0e00422885d80b3c660
#[context("Postprocessing embedded overlayfs")]
fn postprocess_embedded_ovl_whiteouts(root: &Dir) -> Result<()> {
    const OSTREE_WHITEOUT_PREFIX: &str = ".ostree-wh.";
    fn recurse(root: &Dir, path: &Utf8Path) -> Result<u32> {
        let mut n = 0;
        for entry in root.read_dir(path)? {
            let entry = entry?;
            let meta = entry.metadata()?;
            let name = PathBuf::from(entry.file_name());
            let name: Utf8PathBuf = name.try_into()?;
            if meta.is_dir() {
                n += recurse(root, &path.join(name))?;
                continue;
            }
            if !is_overlay_whiteout(&meta) {
                continue;
            };
            let srcpath = path.join(&name);
            let targetname = format!("{OSTREE_WHITEOUT_PREFIX}{name}");
            let destpath = path.join(&targetname);
            root.remove_file(srcpath)?;
            root.atomic_write_with_perms(destpath, "", meta.permissions())?;
            n += 1;
        }
        Ok(n)
    }
    let n = recurse(root, ".".into())?;
    if n > 0 {
        println!("Processed {n} embedded whiteouts");
    } else {
        println!("No embedded whiteouts found");
    }
    Ok(())
}

/// Write an RPM macro file to ensure the rpmdb path is set on the client side.
pub fn compose_postprocess_rpm_macro(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
    postprocess_rpm_macro(rootfs)?;
    Ok(())
}

/// Ensure our own `_dbpath` macro exists in the tree.
#[context("Writing _dbpath RPM macro")]
fn postprocess_rpm_macro(rootfs_dfd: &Dir) -> Result<()> {
    static RPM_MACROS_DIR: &str = "usr/lib/rpm/macros.d";
    static MACRO_FILENAME: &str = "macros.rpm-ostree";
    let mut db = cap_std::fs::DirBuilder::new();
    db.recursive(true);
    db.mode(0o755);
    rootfs_dfd.create_dir_with(RPM_MACROS_DIR, &db)?;
    let rpm_macros_dfd = rootfs_dfd.open_dir(RPM_MACROS_DIR)?;
    let perms = cap_std::fs::Permissions::from_mode(0o644);
    rpm_macros_dfd.atomic_replace_with(&MACRO_FILENAME, |w| -> Result<()> {
        w.get_mut().as_file_mut().set_permissions(perms)?;
        w.write_all(b"%_dbpath /")?;
        w.write_all(RPMOSTREE_RPMDB_LOCATION.as_bytes())?;
        w.write_all(b"\n")?;
        Ok(())
    })?;
    Ok(())
}

// This function does two things: (1) make sure there is a /home --> /var/home substitution rule,
// and (2) make sure there *isn't* a /var/home -> /home substition rule. The latter check won't
// technically be needed once downstreams have:
// https://src.fedoraproject.org/rpms/selinux-policy/pull-request/14
#[context("Postprocessing subs_dist")]
fn postprocess_subs_dist(rootfs_dfd: &Dir) -> Result<()> {
    let path = Path::new("usr/etc/selinux/targeted/contexts/files/file_contexts.subs_dist");
    if let Some(f) = rootfs_dfd.open_optional(path)? {
        let perms = cap_std::fs::Permissions::from_mode(0o644);
        rootfs_dfd.atomic_replace_with(&path, |w| -> Result<()> {
            w.get_mut().as_file_mut().set_permissions(perms)?;
            let f = BufReader::new(&f);
            for line in f.lines() {
                let line = line?;
                if line.starts_with("/var/home ") {
                    writeln!(w, "# https://github.com/projectatomic/rpm-ostree/pull/1754")?;
                    write!(w, "# ")?;
                }
                writeln!(w, "{}", line)?;
            }
            writeln!(w, "# https://github.com/projectatomic/rpm-ostree/pull/1754")?;
            writeln!(w, "/home /var/home")?;
            writeln!(w, "# https://github.com/coreos/rpm-ostree/pull/4640")?;
            writeln!(w, "/usr/etc /etc")?;
            writeln!(w, "# https://github.com/coreos/rpm-ostree/pull/1795")?;
            writeln!(w, "/usr/lib/opt /opt")?;
            Ok(())
        })?;
    }
    Ok(())
}

#[context("Cleaning up rpmdb leftovers")]
fn postprocess_cleanup_rpmdb_impl(rootfs_dfd: &Dir) -> Result<()> {
    let d = if let Some(d) = rootfs_dfd.open_dir_optional(RPMOSTREE_RPMDB_LOCATION)? {
        Utf8Dir::from_cap_std(d)
    } else {
        return Ok(());
    };
    for ent in d.entries()? {
        let ent = ent?;
        let name = ent.file_name()?;
        let name = name.as_str();
        if matches!(name, ".dbenv.lock" | ".rpm.lock") || name.starts_with("__db.") {
            d.remove_file(name)?;
        }
    }
    Ok(())
}

pub(crate) fn postprocess_cleanup_rpmdb(rootfs_dfd: i32) -> CxxResult<()> {
    postprocess_cleanup_rpmdb_impl(unsafe { &ffi_dirfd(rootfs_dfd)? }).map_err(Into::into)
}

/// Final processing steps.
///
/// This function is called from rpmostree_postprocess_final(); think of
/// it as the bits of that function that we've chosen to implement in Rust.
/// It takes care of all things that are really required to use rpm-ostree
/// on the target host.
pub fn compose_postprocess_final_pre(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs_dfd = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
    // These tasks can safely run in parallel, so just for fun we do so via rayon.
    let tasks = [
        postprocess_useradd,
        postprocess_subs_dist,
        postprocess_rpm_macro,
    ];
    tasks.par_iter().try_for_each(|f| f(rootfs_dfd))?;
    // This task recursively traverses the filesystem and hence should be serial.
    postprocess_embedded_ovl_whiteouts(rootfs_dfd)?;
    Ok(())
}

#[context("Handling treefile 'units'")]
fn compose_postprocess_units(rootfs_dfd: &Dir, treefile: &mut Treefile) -> Result<()> {
    let mut db = cap_std::fs::DirBuilder::new();
    db.recursive(true);
    db.mode(0o755);
    let units = if let Some(u) = treefile.parsed.base.units.as_ref() {
        u
    } else {
        return Ok(());
    };
    let multiuser_wants = Path::new("usr/etc/systemd/system/multi-user.target.wants");
    // Sanity check
    if !rootfs_dfd.try_exists("usr/etc")? {
        return Err(anyhow!("Missing usr/etc in rootfs"));
    }
    rootfs_dfd.ensure_dir_with(multiuser_wants, &db)?;

    for unit in units {
        let dest = multiuser_wants.join(unit);
        if rootfs_dfd
            .symlink_metadata_optional(&dest)
            .with_context(|| format!("Querying {unit}"))?
            .is_some()
        {
            continue;
        }

        println!("Adding {} to multi-user.target.wants", unit);
        let target = format!("/usr/lib/systemd/system/{unit}");
        cap_primitives::fs::symlink_contents(target, &rootfs_dfd.as_filelike_view(), dest)
            .with_context(|| format!("Linking {unit}"))?;
    }
    Ok(())
}

#[context("Handling treefile 'default-target'")]
fn compose_postprocess_default_target(rootfs: &Dir, target: &str) -> Result<()> {
    /* This used to be in /etc, but doing it in /usr makes more sense, as it's
     * part of the OS defaults. This was changed in particular to work with
     * ConditionFirstBoot= which runs `systemctl preset-all`:
     * https://github.com/projectatomic/rpm-ostree/pull/1425
     */
    let default_target_path = "usr/lib/systemd/system/default.target";
    rootfs.remove_file_optional(default_target_path)?;
    rootfs.symlink(target, default_target_path)?;
    Ok(())
}

/// The treefile format has two kinds of postprocessing scripts;
/// there's a single `postprocess-script` as well as inline (anonymous)
/// scripts.  This function executes both kinds in bwrap containers.
fn compose_postprocess_scripts(
    rootfs_dfd: &Dir,
    treefile: &mut Treefile,
    unified_core: bool,
) -> Result<()> {
    // Execute the anonymous (inline) scripts.
    for (i, script) in treefile
        .parsed
        .base
        .postprocess
        .iter()
        .flatten()
        .enumerate()
    {
        let binpath = format!("/usr/bin/rpmostree-postprocess-inline-{}", i);
        let target_binpath = &binpath[1..];

        rootfs_dfd.atomic_write_with_perms(
            target_binpath,
            script,
            Permissions::from_mode(0o755),
        )?;
        println!("Executing `postprocess` inline script '{}'", i);
        let child_argv = vec![binpath.to_string()];
        let _ =
            bwrap::bubblewrap_run_sync(rootfs_dfd.as_raw_fd(), &child_argv, false, unified_core)?;
        rootfs_dfd.remove_file(target_binpath)?;
    }

    // And the single postprocess script.
    if let Some(postprocess_script) = treefile.get_postprocess_script() {
        let binpath = "/usr/bin/rpmostree-treefile-postprocess-script";
        let target_binpath = &binpath[1..];
        postprocess_script.seek(std::io::SeekFrom::Start(0))?;
        let mut reader = std::io::BufReader::new(postprocess_script);
        rootfs_dfd.atomic_replace_with(target_binpath, |w| {
            std::io::copy(&mut reader, w)?;
            w.get_mut()
                .as_file_mut()
                .set_permissions(Permissions::from_mode(0o755))?;
            Ok::<_, anyhow::Error>(())
        })?;
        println!("Executing postprocessing script");

        let child_argv = &vec![binpath.to_string()];
        let _ = crate::bwrap::bubblewrap_run_sync(
            rootfs_dfd.as_raw_fd(),
            child_argv,
            false,
            unified_core,
        )
        .context("Executing postprocessing script")?;

        rootfs_dfd.remove_file(target_binpath)?;
        println!("Finished postprocessing script");
    }
    Ok(())
}

/// Logic for handling treefile `remove-files`.
#[context("Handling `remove-files`")]
pub fn compose_postprocess_remove_files(
    rootfs_dfd: &Dir,
    treefile: &mut Treefile,
) -> CxxResult<()> {
    for name in treefile.parsed.base.remove_files.iter().flatten() {
        let p = Path::new(name);
        if p.is_absolute() {
            return Err(anyhow!("Invalid absolute path: {}", name).into());
        }
        if name.contains("..") {
            return Err(anyhow!("Invalid '..' in path: {}", name).into());
        }
        println!("Deleting: {}", name);
        rootfs_dfd.remove_all_optional(name)?;
    }
    Ok(())
}

fn compose_postprocess_add_files(rootfs_dfd: &Dir, treefile: &mut Treefile) -> Result<()> {
    let mut db = cap_std::fs::DirBuilder::new();
    db.recursive(true);
    db.mode(0o755);
    // Make a deep copy here because get_add_file_fd() also wants an &mut
    // reference.
    let add_files: Vec<_> = treefile
        .parsed
        .base
        .add_files
        .iter()
        .flatten()
        .cloned()
        .collect();
    for (src, dest) in add_files {
        let reldest = dest.trim_start_matches('/');
        if reldest.is_empty() {
            return Err(anyhow!("Invalid add-files destination: {}", dest));
        }
        let dest = if reldest.starts_with("etc/") {
            Cow::Owned(format!("usr/{}", reldest))
        } else {
            Cow::Borrowed(reldest)
        };

        println!("Adding file {}", dest);
        let dest = Path::new(&*dest);
        if let Some(parent) = dest.parent() {
            rootfs_dfd.ensure_dir_with(parent, &db)?;
        }

        let fd = treefile.get_add_file(&src);
        fd.seek(std::io::SeekFrom::Start(0))?;
        let mut reader = std::io::BufReader::new(fd);
        let perms = reader.get_mut().metadata()?.permissions();
        rootfs_dfd.atomic_replace_with(dest, |w| {
            std::io::copy(&mut reader, w)?;
            w.get_mut()
                .as_file_mut()
                .set_permissions(cap_std::fs::Permissions::from_std(perms))?;
            Ok::<_, anyhow::Error>(())
        })?;
    }
    Ok(())
}

#[context("Symlinking {}", TRADITIONAL_RPMDB_LOCATION)]
fn compose_postprocess_rpmdb(rootfs_dfd: &Dir) -> Result<()> {
    /* This works around a potential issue with libsolv if we go down the
     * rpmostree_get_pkglist_for_root() path. Though rpm has been using the
     * /usr/share/rpm location (since the RpmOstreeContext set the _dbpath macro),
     * the /var/lib/rpm directory will still exist, but be empty. libsolv gets
     * confused because it sees the /var/lib/rpm dir and doesn't even try the
     * /usr/share/rpm location, and eventually dies when it tries to load the
     * data. XXX: should probably send a patch upstream to libsolv.
     *
     * So we set the symlink now. This is also what we do on boot anyway for
     * compatibility reasons using tmpfiles.
     * */
    rootfs_dfd.remove_all_optional(TRADITIONAL_RPMDB_LOCATION)?;
    rootfs_dfd.symlink(
        format!("../../{}", RPMOSTREE_RPMDB_LOCATION),
        TRADITIONAL_RPMDB_LOCATION,
    )?;
    Ok(())
}

/// Enables ostree-state-overlay@.service for /usr/lib/opt and /usr/local. These
/// symlinks are also used later in the compose process (and client-side composes)
/// as a way to check that state overlays are turned on.
fn compose_postprocess_state_overlays(rootfs_dfd: &Dir) -> Result<()> {
    let mut db = cap_std::fs::DirBuilder::new();
    db.recursive(true);
    db.mode(0o755);
    let localfs_requires = Path::new(SD_LOCAL_FS_TARGET_REQUIRES);
    rootfs_dfd.ensure_dir_with(localfs_requires, &db)?;

    const UNITS: &[&str] = &[
        "ostree-state-overlay@usr-lib-opt.service",
        "ostree-state-overlay@usr-local.service",
    ];

    UNITS.par_iter().try_for_each(|&unit| {
        let target = Path::new("..").join(unit);
        let linkpath = localfs_requires.join(unit);
        rootfs_dfd
            .symlink(target, linkpath)
            .with_context(|| format!("Enabling {unit}"))
    })?;

    Ok(())
}

/// Rust portion of rpmostree_treefile_postprocessing()
pub fn compose_postprocess(
    rootfs_dfd: i32,
    treefile: &mut Treefile,
    next_version: &str,
    unified_core: bool,
) -> CxxResult<()> {
    let rootfs = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };

    // One of several dances we do around this that really needs to be completely
    // reworked.
    if rootfs.try_exists("etc")? {
        rootfs.rename("etc", rootfs, "usr/etc")?;
    }

    compose_postprocess_rpmdb(rootfs)?;
    compose_postprocess_units(rootfs, treefile)?;
    if let Some(t) = treefile.parsed.base.default_target.as_deref() {
        compose_postprocess_default_target(rootfs, t)?;
    }

    if treefile
        .parsed
        .base
        .opt_usrlocal_overlays
        .unwrap_or_default()
    {
        compose_postprocess_state_overlays(rootfs)?;
    }

    treefile.write_compose_json(rootfs)?;

    let etc_guard = crate::core::prepare_tempetc_guard(rootfs_dfd.as_raw_fd())?;
    // These ones depend on the /etc path
    compose_postprocess_mutate_os_release(rootfs, treefile, next_version)?;
    compose_postprocess_remove_files(rootfs, treefile)?;
    compose_postprocess_add_files(rootfs, treefile)?;
    etc_guard.undo()?;

    compose_postprocess_scripts(rootfs, treefile, unified_core)?;

    Ok(())
}

/// Implementation of the treefile `mutate-os-release` field.
#[context("Updating os-release with commit version")]
fn compose_postprocess_mutate_os_release(
    rootfs: &Dir,
    treefile: &mut Treefile,
    next_version: &str,
) -> Result<()> {
    let base_version = if let Some(base_version) = treefile.parsed.base.mutate_os_release.as_deref()
    {
        base_version
    } else {
        return Ok(());
    };
    if next_version.is_empty() {
        println!("Ignoring mutate-os-release: no commit version specified.");
        return Ok(());
    }
    // find the real path to os-release using bwrap; this is an overkill but safer way
    // of resolving a symlink relative to a rootfs (see discussions in
    // https://github.com/projectatomic/rpm-ostree/pull/410/)
    let mut bwrap = crate::bwrap::Bubblewrap::new_with_mutability(
        &rootfs,
        crate::ffi::BubblewrapMutability::Immutable,
    )?;
    bwrap.append_child_argv(["realpath", "/etc/os-release"]);
    let cancellable = &gio::Cancellable::new();
    let cancellable = Some(cancellable);
    let path = bwrap.run_captured(cancellable)?;
    let path = std::str::from_utf8(&path)
        .context("Parsing realpath")?
        .trim_start_matches('/')
        .trim_end();
    let path = if path.is_empty() {
        // fallback on just overwriting etc/os-release
        "etc/os-release"
    } else {
        path
    };
    println!("Updating {}", path);
    let contents = rootfs
        .read_to_string(path)
        .with_context(|| format!("Reading {path}"))?;
    let new_contents = mutate_os_release_contents(&contents, base_version, next_version);
    rootfs
        .atomic_write(path, new_contents.as_bytes())
        .with_context(|| format!("Writing {path}"))?;
    Ok(())
}

/// Given the contents of a /usr/lib/os-release file,
/// update the `VERSION` and `PRETTY_NAME` fields.
fn mutate_os_release_contents(contents: &str, base_version: &str, next_version: &str) -> String {
    let mut buf = String::new();
    for line in contents.lines() {
        if line.is_empty() {
            continue;
        }
        let prefixes = &["VERSION=", "PRETTY_NAME="];
        if let Some((prefix, rest)) = strip_any_prefix(line, prefixes) {
            buf.push_str(prefix);
            let replaced = rest.replace(base_version, next_version);
            buf.push_str(&replaced);
        } else {
            buf.push_str(line);
        }
        buf.push('\n');
    }

    let quoted_version = glib::shell_quote(next_version);
    let quoted_version = quoted_version.to_str().unwrap();
    // Unwrap safety: write! to a String can't fail
    writeln!(buf, "OSTREE_VERSION={}", quoted_version).unwrap();

    buf
}

/// Given a string and a set of possible prefixes, return the split
/// prefix and remaining string, or `None` if no matches.
fn strip_any_prefix<'a, 'b>(s: &'a str, prefixes: &[&'b str]) -> Option<(&'b str, &'a str)> {
    prefixes
        .iter()
        .find_map(|&p| s.strip_prefix(p).map(|r| (p, r)))
}

/// Inject `altfiles` after `files` for `passwd:` and `group:` entries.
fn add_altfiles(buf: &str) -> Result<String> {
    let mut r = String::with_capacity(buf.len());
    for line in buf.lines() {
        let parts = if let Some(p) = strip_any_prefix(line, &["passwd:", "group:"]) {
            p
        } else {
            r.push_str(line);
            r.push('\n');
            continue;
        };
        let (prefix, rest) = parts;
        r.push_str(prefix);

        let mut inserted = false;
        for elt in rest.split_whitespace() {
            // Already have altfiles?  We're done
            if elt == "altfiles" {
                return Ok(buf.to_string());
            }
            // We prefer `files altfiles`
            if !inserted && elt == "files" {
                r.push_str(" files altfiles");
                inserted = true;
            } else {
                r.push(' ');
                r.push_str(elt);
            }
        }
        if !inserted {
            r.push_str(" altfiles");
        }
        r.push('\n');
    }
    Ok(r)
}

/// Add `altfiles` entries to `nsswitch.conf`.
///
/// rpm-ostree currently depends on `altfiles`
#[context("Adding altfiles to /etc/nsswitch.conf")]
pub fn composepost_nsswitch_altfiles(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs_dfd = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
    let path = "usr/etc/nsswitch.conf";
    if let Some(meta) = rootfs_dfd.symlink_metadata_optional(path)? {
        // If it's a symlink, then something else e.g. authselect must own it.
        if meta.is_symlink() {
            return Ok(());
        }
        let nsswitch = rootfs_dfd.read_to_string(path)?;
        let nsswitch = add_altfiles(&nsswitch)?;
        rootfs_dfd.atomic_write(path, nsswitch.as_bytes())?;
    }

    Ok(())
}

/// Go over `/var` in the rootfs and convert them to tmpfiles.d entries. Only directories and
/// symlinks are handled. rpm-ostree itself creates some symlinks for various reasons.
///
/// In the non-unified core path, conversion is necessary to ensure that (1) any subdirs/symlinks
/// from the RPM itself and (2) any subdirs/symlinks from scriptlets will be created on first boot.
/// In the unified core path, (1) is handled by the importer, and (2) is blocked by bwrap, so it's
/// really just for rpm-ostree-created bits itself.
///
/// In theory, once we drop non-unified core support, we should be able to drop this and make those
/// few rpm-ostree compat symlinks just directly write tmpfiles.d dropins.
pub fn convert_var_to_tmpfiles_d(
    rootfs_dfd: i32,
    cancellable: &crate::FFIGCancellable,
) -> CxxResult<()> {
    let rootfs = unsafe { crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
    let cancellable = &cancellable.glib_reborrow();

    // TODO(lucab): unify this logic with the one in rpmostree-importer.cxx.
    var_to_tmpfiles(&rootfs, Some(cancellable))?;
    Ok(())
}

#[context("Converting /var to tmpfiles.d")]
fn var_to_tmpfiles(rootfs: &Dir, cancellable: Option<&gio::Cancellable>) -> Result<()> {
    /* List of files that are known to possibly exist, but in practice
     * things work fine if we simply ignore them.  Don't add something
     * to this list unless you've verified it's handled correctly at
     * runtime.  (And really both in CentOS and Fedora)
     */
    static KNOWN_STATE_FILES: &[&str] = &[
        // https://bugzilla.redhat.com/show_bug.cgi?id=789407
        "var/lib/systemd/random-seed",
        "var/lib/systemd/catalog/database",
        "var/lib/plymouth/boot-duration",
        // These two are part of systemd's var.tmp
        "var/log/wtmp",
        "var/log/btmp",
    ];

    let pwdb = PasswdDB::populate_new(rootfs)?;

    // We never want to traverse into /run when making tmpfiles since it's a tmpfs
    // Note that in a Fedora root, /var/run is a symlink, though on el7, it can be a dir.
    // See: https://github.com/projectatomic/rpm-ostree/pull/831
    if rootfs.try_exists("var/run")? {
        rootfs
            .remove_dir_all("var/run")
            .context("Failed to remove /var/run")?;
    }

    // Here, delete some files ahead of time to avoid emitting warnings
    // for things that are known to be harmless.
    for path in KNOWN_STATE_FILES {
        rootfs
            .remove_file_optional(*path)
            .with_context(|| format!("unlinkat({})", path))?;
    }

    // Convert /var wholesale to tmpfiles.d. Note that with unified core, this
    // code should no longer be necessary as we convert packages on import.
    // Make output file world-readable, no reason why not to
    // https://bugzilla.redhat.com/show_bug.cgi?id=1631794
    let mut db = cap_std::fs::DirBuilder::new();
    db.recursive(true);
    db.mode(0o755);
    rootfs.create_dir_with("usr/lib/tmpfiles.d", &db)?;
    let mode = Permissions::from_mode(0o644);
    rootfs.atomic_replace_with(
        "usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf",
        |bufwr| -> Result<()> {
            bufwr.get_mut().as_file_mut().set_permissions(mode)?;
            let mut prefix = Utf8PathBuf::from("var");
            let mut entries = BTreeSet::new();
            convert_path_to_tmpfiles_d_recurse(
                &mut entries,
                &pwdb,
                rootfs,
                &mut prefix,
                &cancellable,
            )
            .with_context(|| format!("Processing var content /{}", prefix))?;
            for line in entries {
                bufwr.write_all(line.as_bytes())?;
                writeln!(bufwr)?;
            }
            Ok(())
        },
    )?;

    Ok(())
}

/// Recursively explore target directory and translate content to tmpfiles.d entries. See
/// `convert_var_to_tmpfiles_d` for more background.
///
/// This proceeds depth-first and progressively deletes translated subpaths as it goes.
/// `prefix` is updated at each recursive step, so that in case of errors it can be
/// used to pinpoint the faulty path.
#[allow(clippy::nonminimal_bool)]
fn convert_path_to_tmpfiles_d_recurse(
    out_entries: &mut BTreeSet<String>,
    pwdb: &PasswdDB,
    rootfs: &Dir,
    prefix: &mut Utf8PathBuf,
    cancellable: &Option<&gio::Cancellable>,
) -> Result<()> {
    let current_prefix = prefix.clone();
    for subpath in rootfs.read_dir(&current_prefix).context("Reading dir")? {
        if let Some(c) = cancellable {
            c.set_error_if_cancelled()?;
        }

        let subpath = subpath?;
        let meta = subpath.metadata()?;
        let fname: Utf8PathBuf = PathBuf::from(subpath.file_name()).try_into()?;
        let full_path = Utf8Path::new(&current_prefix).join(&fname);

        // Workaround for nfs-utils in RHEL7:
        // https://bugzilla.redhat.com/show_bug.cgi?id=1427537
        let retain_entry = meta.is_file() && full_path.starts_with("var/lib/nfs");
        if !retain_entry && !(meta.is_dir() || meta.is_symlink()) {
            rootfs
                .remove_file_optional(&full_path)
                .with_context(|| format!("Removing {:?}", &full_path))?;
            println!("Ignoring non-directory/non-symlink '{:?}'", &full_path);
            continue;
        }

        // Translate this file entry.
        let entry = {
            let mode = meta.mode() & !libc::S_IFMT;

            let file_info = gio::FileInfo::new();
            file_info.set_attribute_uint32("unix::mode", mode);

            if meta.is_dir() {
                file_info.set_file_type(FileType::Directory);
            } else if meta.is_symlink() {
                file_info.set_file_type(FileType::SymbolicLink);
                let link_target = cap_primitives::fs::read_link_contents(
                    &rootfs.as_filelike_view(),
                    full_path.as_std_path(),
                )
                .context("Reading symlink")?;
                let link_target = link_target
                    .to_str()
                    .ok_or_else(|| format_err!("non UTF-8 symlink target '{:?}'", link_target))?;
                file_info.set_symlink_target(link_target);
            } else if meta.is_file() {
                file_info.set_file_type(FileType::Regular);
            } else {
                unreachable!("invalid path type: {:?}", full_path);
            }

            let abs_path = Utf8Path::new("/").join(&full_path);
            let username = pwdb.lookup_user(meta.uid())?;
            let groupname = pwdb.lookup_group(meta.gid())?;
            importer::translate_to_tmpfiles_d(abs_path.as_str(), &file_info, &username, &groupname)?
        };
        out_entries.insert(entry);

        if meta.is_dir() {
            // New subdirectory discovered, recurse into it.
            *prefix = full_path.clone();
            convert_path_to_tmpfiles_d_recurse(out_entries, pwdb, rootfs, prefix, cancellable)?;
            rootfs.remove_dir_all(&full_path)?;
        } else {
            rootfs.remove_file(&full_path)?;
        }
    }
    Ok(())
}

fn state_overlay_enabled(rootfs_dfd: &cap_std::fs::Dir, state_overlay: &str) -> Result<bool> {
    let linkname =
        format!("{SD_LOCAL_FS_TARGET_REQUIRES}/ostree-state-overlay@{state_overlay}.service");
    match rootfs_dfd.symlink_metadata_optional(&linkname)? {
        Some(meta) if meta.is_symlink() => Ok(true),
        Some(_) => Err(anyhow!("{linkname} is not a symlink")),
        None => Ok(false),
    }
}

/// Walk over the root filesystem and perform some core conversions
/// from RPM conventions to OSTree conventions.
///
/// For example:
///  - Symlink /usr/local -> /var/usrlocal
///  - If present, symlink /var/lib/alternatives -> /usr/lib/alternatives
///  - If present, symlink /var/lib/vagrant -> /usr/lib/vagrant
#[context("Preparing symlinks in rootfs")]
pub fn rootfs_prepare_links(rootfs_dfd: i32, skip_usrlocal: bool) -> CxxResult<()> {
    let rootfs = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
    let mut db = dirbuilder_from_mode(0o755);
    db.recursive(true);

    if !skip_usrlocal {
        if state_overlay_enabled(rootfs, "usr-local")? {
            // because of the filesystem lua issue (see
            // compose_init_rootfs_base()) we need to create this manually
            rootfs.ensure_dir_with("usr/local", &db)?;
        } else if !crate::ostree_prepareroot::transient_root_enabled(rootfs)? {
            // Unconditionally drop /usr/local and replace it with a symlink.
            rootfs
                .remove_all_optional("usr/local")
                .context("Removing /usr/local")?;
            ensure_symlink(rootfs, "../var/usrlocal", "usr/local")
                .context("Creating /usr/local symlink")?;
        }
    }

    // Move existing content to /usr/lib, then put a symlink in its
    // place under /var/lib.
    rootfs
        .ensure_dir_with("usr/lib", &db)
        .context("Creating /usr/lib")?;
    for entry in COMPAT_VARLIB_SYMLINKS {
        let varlib_path = format!("var/lib/{}", entry);
        let is_var_dir = rootfs
            .symlink_metadata_optional(&varlib_path)?
            .map(|m| m.is_dir())
            .unwrap_or(false);
        if !is_var_dir {
            continue;
        }

        let usrlib_path = format!("usr/lib/{}", entry);
        rootfs
            .remove_all_optional(&usrlib_path)
            .with_context(|| format!("Removing /{}", &usrlib_path))?;
        rootfs
            .rename(&varlib_path, rootfs, &usrlib_path)
            .with_context(|| format!("Moving /{} to /{}", &varlib_path, &usrlib_path))?;

        let target = format!("../../{}", &usrlib_path);
        ensure_symlink(&rootfs, &target, &varlib_path)
            .with_context(|| format!("Creating /{} symlink", &varlib_path))?;
    }

    Ok(())
}

/// Create a symlink at `linkpath` if it does not exist, pointing to `target`.
///
/// This is idempotent and does not alter any content already existing at `linkpath`.
/// It returns `true` if the symlink has been created, `false` otherwise.
#[context("Symlinking '/{}' to empty directory '/{}'", linkpath, target)]
fn ensure_symlink(rootfs: &Dir, target: &str, linkpath: &str) -> Result<bool> {
    let mut db = dirbuilder_from_mode(0o755);
    db.recursive(true);
    if let Some(meta) = rootfs.symlink_metadata_optional(linkpath)? {
        if meta.is_symlink() {
            // We assume linkpath already points to the correct target,
            // thus this short-circuits in an idempotent way.
            return Ok(false);
        } else if meta.is_dir() {
            rootfs.remove_dir(linkpath)?;
        } else {
            bail!("Content already exists at link path");
        }
    } else {
        // For maximum compatibility, create parent directories too.  This
        // is necessary when we're doing layering on top of a base commit,
        // and the /var will be empty.  We should probably consider running
        // systemd-tmpfiles to setup the temporary /var.
        if let Some(parent) = Path::new(linkpath).parent() {
            rootfs.ensure_dir_with(parent, &db)?;
        }
    }

    rootfs.symlink(target, linkpath)?;
    Ok(true)
}

pub fn workaround_selinux_cross_labeling(
    rootfs_dfd: i32,
    cancellable: Pin<&mut crate::FFIGCancellable>,
) -> CxxResult<()> {
    let rootfs = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };
    let cancellable = &cancellable.gobj_wrap();

    tweak_selinux_timestamps(rootfs, Some(cancellable))?;
    Ok(())
}

/// Tweak timestamps on SELinux policy (workaround cross-host leak).
///
/// SELinux uses PCRE pre-compiled regexps for binary caches, which can
/// fail if the version of PCRE on the host differs from the version
/// which generated the cache (in the target root).
///
/// Note also this function is probably already broken in Fedora
/// 23+ from https://bugzilla.redhat.com/show_bug.cgi?id=1265406
#[context("Tweaking SELinux timestamps")]
fn tweak_selinux_timestamps(rootfs: &Dir, cancellable: Option<&gio::Cancellable>) -> Result<()> {
    // Handle the policy being in both /usr/etc and /etc since
    // this function can be called at different points.
    let policy_path = if rootfs.try_exists("usr/etc")? {
        "usr/etc/selinux"
    } else {
        "etc/selinux"
    };

    if rootfs.try_exists(policy_path)? {
        let mut prefix = policy_path.to_string();
        workaround_selinux_cross_labeling_recurse(rootfs, &mut prefix, &cancellable)
            .with_context(|| format!("Analyzing /{} content", prefix))?;
    }

    Ok(())
}

/// Recursively explore target directory and tweak SELinux policy timestamps.
///
/// `prefix` is updated at each recursive step, so that in case of errors it can be
/// used to pinpoint the faulty path.
fn workaround_selinux_cross_labeling_recurse(
    rootfs: &Dir,
    prefix: &mut String,
    cancellable: &Option<&gio::Cancellable>,
) -> Result<()> {
    let current_prefix = prefix.clone();
    for subpath in rootfs.read_dir(&current_prefix)? {
        if let Some(c) = cancellable {
            c.set_error_if_cancelled()?;
        }

        let subpath = subpath?;
        let full_path = {
            let fname = subpath.file_name();
            let path_name = fname
                .to_str()
                .ok_or_else(|| anyhow!("invalid non-UTF-8 path: {:?}", fname))?;
            format!("{}/{}", &current_prefix, &path_name)
        };

        if subpath.file_type()?.is_dir() {
            // New subdirectory discovered, recurse into it.
            *prefix = full_path.clone();
            workaround_selinux_cross_labeling_recurse(rootfs, prefix, cancellable)?;
        } else if let Some(nonbin_name) = full_path.strip_suffix(".bin") {
            rootfs
                .update_timestamps(nonbin_name)
                .with_context(|| format!("Updating timestamps of /{}", nonbin_name))?;
        }
    }
    Ok(())
}

/// This is the nearly the last code executed before we run `ostree commit`.
pub fn compose_postprocess_final(rootfs_dfd: i32, _treefile: &Treefile) -> CxxResult<()> {
    let rootfs = unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? };

    hardlink_rpmdb_base_location(rootfs, None)?;
    Ok(())
}

/// Recurse into this directory and return the total size of all regular files.
#[context("Computing directory size")]
pub fn directory_size(dfd: i32, cancellable: &crate::FFIGCancellable) -> CxxResult<u64> {
    let cancellable = cancellable.glib_reborrow();
    let dfd = unsafe { &crate::ffiutil::ffi_dirfd(dfd)? };
    fn directory_size_recurse(d: &Dir, cancellable: &gio::Cancellable) -> Result<u64> {
        let mut r = 0;
        for ent in d.entries()? {
            cancellable.set_error_if_cancelled()?;
            let ent = ent?;
            let meta = ent
                .metadata()
                .with_context(|| format!("Failed to access {:?}", ent.file_name()))?;
            if meta.is_dir() {
                let child = d.open_dir(ent.file_name())?;
                r += directory_size_recurse(&child, cancellable)?;
            } else if meta.is_file() {
                r += meta.size() as u64;
            }
        }
        Ok(r)
    }
    Ok(directory_size_recurse(&dfd, &cancellable)?)
}

#[context("Hardlinking rpmdb to base location")]
fn hardlink_rpmdb_base_location(
    rootfs: &Dir,
    cancellable: Option<&gio::Cancellable>,
) -> Result<bool> {
    if !rootfs.try_exists(RPMOSTREE_RPMDB_LOCATION)? {
        return Ok(false);
    }

    // Hardlink our own `/usr/lib/sysimage/rpm-ostree-base-db/` hierarchy
    // to the well-known `/usr/share/rpm/`.
    let mut db = dirbuilder_from_mode(0o755);
    db.recursive(true);
    rootfs.ensure_dir_with(RPMOSTREE_BASE_RPMDB, &db)?;
    let perms = Permissions::from_mode(0o755);
    rootfs.set_permissions(RPMOSTREE_BASE_RPMDB, perms)?;
    hardlink_hierarchy(
        rootfs,
        RPMOSTREE_RPMDB_LOCATION,
        RPMOSTREE_BASE_RPMDB,
        cancellable,
    )?;

    // And write a symlink from the proposed standard /usr/lib/sysimage/rpm
    // to our /usr/share/rpm - eventually we will invert this.

    // Temporarily remove the directory if it exists until then.
    // Also, delete a stamp file created by https://src.fedoraproject.org/rpms/rpm/c/391c3aeb66e8c2a0ac684580ac82c41d7da2128b?branch=rawhide
    let stampfile = &Path::new(RPMOSTREE_SYSIMAGE_RPMDB).join(".rpmdbdirsymlink_created");
    rootfs.remove_file_optional(stampfile)?;
    rootfs.remove_all_optional(RPMOSTREE_SYSIMAGE_RPMDB)?;
    rootfs.symlink("../../share/rpm", RPMOSTREE_SYSIMAGE_RPMDB)?;

    Ok(true)
}

#[context("Rewriting rpmdb for target native format")]
fn rewrite_rpmdb_for_target_inner(rootfs_dfd: &Dir, normalize: bool) -> Result<()> {
    let tempetc = crate::core::prepare_tempetc_guard(rootfs_dfd.as_raw_fd())?;

    let mut dbfd = cap_std_ext::cap_tempfile::TempFile::new_anonymous(rootfs_dfd)?;

    let dbpath_arg = format!("--dbpath=/proc/self/cwd/{}", RPMOSTREE_RPMDB_LOCATION);
    // Fork rpmdb from the *host* rootfs to read the rpmdb back into memory
    let r = std::process::Command::new("rpmdb")
        .args(&[dbpath_arg.as_str(), "--exportdb"])
        .current_dir(format!("/proc/self/fd/{}", rootfs_dfd.as_raw_fd()))
        .stdout(Stdio::from(dbfd.try_clone()?))
        .status()?;
    if !r.success() {
        return Err(anyhow!("Failed to execute rpmdb --exportdb: {:?}", r));
    }

    // Clear out the db on disk
    rootfs_dfd.remove_all_optional(RPMOSTREE_RPMDB_LOCATION)?;
    let db = dirbuilder_from_mode(0o755);
    rootfs_dfd.create_dir_with(RPMOSTREE_RPMDB_LOCATION, &db)?;

    // Only one owner now
    dbfd.seek(std::io::SeekFrom::Start(0))?;

    // In the interests of build stability, rewrite the INSTALLTIME and INSTALLTID tags
    // to be deterministic and dervied from `SOURCE_DATE_EPOCH` if requested.
    if normalize {
        normalization::rewrite_rpmdb_timestamps(&mut dbfd)?;
    }

    // Fork the target rpmdb to write the content from memory to disk
    let mut bwrap = Bubblewrap::new_with_mutability(&rootfs_dfd, BubblewrapMutability::RoFiles)?;
    bwrap.append_child_argv(["rpmdb", dbpath_arg.as_str(), "--importdb"]);
    bwrap.take_stdin_fd(dbfd.into_raw_fd());
    let cancellable = gio::Cancellable::new();
    bwrap
        .run(cancellable.reborrow_cxx())
        .context("Failed to run rpmdb --importdb")?;

    // Sometimes we can end up with build-to-build variance in the underlying rpmdb
    // files. Attempt to sort that out, if requested.
    if normalize {
        normalization::normalize_rpmdb(&rootfs_dfd, RPMOSTREE_RPMDB_LOCATION)?;
    }

    tempetc.undo()?;

    Ok(())
}

pub(crate) fn rewrite_rpmdb_for_target(rootfs_dfd: i32, normalize: bool) -> CxxResult<()> {
    Ok(rewrite_rpmdb_for_target_inner(
        unsafe { &crate::ffiutil::ffi_dirfd(rootfs_dfd)? },
        normalize,
    )?)
}

/// Recursively hard-link `source` hierarchy to `target` directory.
///
/// Both directories must exist beforehand.
#[context("Hardlinking /{} to /{}", source, target)]
fn hardlink_hierarchy(
    rootfs: &Dir,
    source: &str,
    target: &str,
    cancellable: Option<&gio::Cancellable>,
) -> Result<()> {
    let mut prefix = "".to_string();
    hardlink_recurse(rootfs, source, target, &mut prefix, &cancellable)
        .with_context(|| format!("Analyzing /{}/{} content", source, prefix))?;

    Ok(())
}

/// Recursively hard-link `source_prefix` to `dest_prefix.`
///
/// `relative_path` is updated at each recursive step, so that in case of errors
/// it can be used to pinpoint the faulty path.
fn hardlink_recurse(
    rootfs: &Dir,
    source_prefix: &str,
    dest_prefix: &str,
    relative_path: &mut String,
    cancellable: &Option<&gio::Cancellable>,
) -> Result<()> {
    let current_dir = relative_path.clone();
    let current_source_dir = format!("{}/{}", source_prefix, relative_path);
    for subpath in rootfs.read_dir(&current_source_dir)? {
        if let Some(c) = cancellable {
            c.set_error_if_cancelled()?;
        }

        let subpath = subpath?;
        let full_path = {
            let fname = subpath.file_name();
            let path_name = fname
                .to_str()
                .ok_or_else(|| anyhow!("invalid non-UTF-8 path: {:?}", fname))?;
            if !current_dir.is_empty() {
                format!("{}/{}", current_dir, path_name)
            } else {
                path_name.to_string()
            }
        };
        let source_path = format!("{}/{}", source_prefix, full_path);
        let dest_path = format!("{}/{}", dest_prefix, full_path);

        if subpath.file_type()?.is_dir() {
            // New subdirectory discovered, create it at the target.
            let perms = rootfs.metadata(&source_path)?.mode() & !libc::S_IFMT;
            let db = dirbuilder_from_mode(perms);
            rootfs.ensure_dir_with(&dest_path, &db)?;
            let perms = Permissions::from_mode(perms);
            rootfs.set_permissions(&dest_path, perms)?;

            // Recurse into the subdirectory.
            *relative_path = full_path.clone();
            hardlink_recurse(
                rootfs,
                source_prefix,
                dest_prefix,
                relative_path,
                cancellable,
            )?;
        } else {
            rustix::fs::linkat(
                rootfs,
                source_path,
                rootfs,
                dest_path,
                rustix::fs::AtFlags::empty(),
            )?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use cap_std::fs::{Dir, DirBuilder};
    use cap_std_ext::{cap_std, cap_tempfile};

    #[test]
    fn stripany() {
        let s = "foo: bar";
        assert!(strip_any_prefix(s, &[]).is_none());
        assert_eq!(
            strip_any_prefix(s, &["baz:", "foo:", "bar:"]).unwrap(),
            ("foo:", " bar")
        );
    }

    #[test]
    fn altfiles_replaced() {
        let orig = r##"# blah blah nss stuff
# more blah blah

# passwd: db files
# shadow: db files
# shadow: db files

passwd:     sss files systemd
shadow:     files
group:      sss files systemd
hosts:      files resolve [!UNAVAIL=return] myhostname dns
automount:  files sss
"##;
        let expected = r##"# blah blah nss stuff
# more blah blah

# passwd: db files
# shadow: db files
# shadow: db files

passwd: sss files altfiles systemd
shadow:     files
group: sss files altfiles systemd
hosts:      files resolve [!UNAVAIL=return] myhostname dns
automount:  files sss
"##;
        let replaced = add_altfiles(orig).unwrap();
        assert_eq!(replaced.as_str(), expected);
        let replaced2 = add_altfiles(replaced.as_str()).unwrap();
        assert_eq!(replaced2.as_str(), expected);
    }

    #[test]
    fn test_mutate_os_release() {
        let orig = r##"NAME=Fedora
VERSION="33 (Container Image)"
ID=fedora
VERSION_ID=33
VERSION_CODENAME=""
PRETTY_NAME="Fedora 33 (Container Image)"
CPE_NAME="cpe:/o:fedoraproject:fedora:33"
"##;
        let expected = r##"NAME=Fedora
VERSION="33.4 (Container Image)"
ID=fedora
VERSION_ID=33
VERSION_CODENAME=""
PRETTY_NAME="Fedora 33.4 (Container Image)"
CPE_NAME="cpe:/o:fedoraproject:fedora:33"
OSTREE_VERSION='33.4'
"##;
        let replaced = mutate_os_release_contents(orig, "33", "33.4");
        assert_eq!(replaced.as_str(), expected);
    }

    fn verify_base(rootfs: &Dir) -> Result<()> {
        // Not exhaustive, just a sanity check
        for d in ["proc", "sys"] {
            assert!(rootfs.symlink_metadata(d)?.is_dir());
        }
        let homelink = rootfs.read_link("home")?;
        assert_eq!(homelink.to_str().unwrap(), "var/home");
        Ok(())
    }

    #[test]
    fn test_init_rootfs_strict() -> Result<()> {
        // Test the legacy tmp_is_dir path
        {
            let rootfs = cap_tempfile::tempdir(cap_tempfile::ambient_authority())?;
            compose_init_rootfs_base(&rootfs, false)?;
            let target = rootfs.read_link("tmp").unwrap();
            assert_eq!(target, Path::new("sysroot/tmp"));
            verify_base(&rootfs)?;
        }
        // Default expected strict mode
        let rootfs = cap_tempfile::tempdir(cap_tempfile::ambient_authority())?;
        compose_init_rootfs_base(&rootfs, true)?;
        let tmpdir_meta = rootfs.metadata("tmp").unwrap();
        assert!(tmpdir_meta.is_dir());
        assert_eq!(tmpdir_meta.permissions().mode() & 0o7777, 0o1777);
        verify_base(&rootfs)?;
        Ok(())
    }

    #[test]
    fn test_init_rootfs_transient() -> Result<()> {
        let rootfs = cap_tempfile::tempdir(cap_tempfile::ambient_authority())?;
        compose_init_rootfs_transient(&rootfs)?;
        let tmpdir_meta = rootfs.metadata("tmp").unwrap();
        assert!(tmpdir_meta.is_dir());
        assert_eq!(tmpdir_meta.permissions().mode() & 0o7777, 0o1777);
        verify_base(&rootfs)?;
        for d in ["opt", "usr/local"] {
            assert!(
                rootfs
                    .symlink_metadata(d)
                    .with_context(|| format!("Verifying {d} is dir"))?
                    .is_dir(),
                "Verifying {d} is dir"
            );
        }
        Ok(())
    }

    #[test]
    fn test_overlay() -> Result<()> {
        // We don't actually test creating whiteout devices here as that
        // may not work.
        let td = cap_tempfile::tempdir(cap_std::ambient_authority())?;
        // Verify no-op case
        postprocess_embedded_ovl_whiteouts(&td).unwrap();
        td.create("foo")?;
        td.symlink("foo", "bar")?;
        postprocess_embedded_ovl_whiteouts(&td).unwrap();
        assert!(td.try_exists("foo")?);
        assert!(td.try_exists("bar")?);

        Ok(())
    }

    #[test]
    fn test_tmpfiles_d_translation() {
        use nix::sys::stat::{umask, Mode};
        use rustix::process::{getegid, geteuid};

        // Create an empty file with the given mode
        fn touch(d: &Dir, p: impl AsRef<Utf8Path>, mode: u32) -> Result<()> {
            d.atomic_replace_with(p.as_ref(), |w| {
                w.get_mut()
                    .as_file_mut()
                    .set_permissions(Permissions::from_mode(mode))
                    .map_err(Into::into)
            })
        }

        let mut db = DirBuilder::new();
        db.recursive(true);
        db.mode(0o755);

        // Prepare a minimal rootfs as playground.
        umask(Mode::empty());
        let rootfs = cap_tempfile::tempdir(cap_std::ambient_authority()).unwrap();
        let uid = geteuid().as_raw();
        let gid = getegid().as_raw();
        let uid_str = format!("{uid}");
        let gid_str = format!("{gid}");
        let mut expected_disk_size = 30u64;
        {
            for dirpath in &["usr/lib", "usr/etc", "var"] {
                rootfs.ensure_dir_with(*dirpath, &db).unwrap();
            }
            for filepath in &["usr/lib/passwd", "usr/lib/group"] {
                touch(&rootfs, *filepath, 0o755).unwrap()
            }
            rootfs
                .atomic_write(
                    "usr/etc/passwd",
                    format!("test-user:x:{uid_str}:{gid_str}:::",),
                )
                .unwrap();
            expected_disk_size += uid_str.len() as u64;
            expected_disk_size += gid_str.len() as u64;
            rootfs
                .atomic_write("usr/etc/group", format!("test-group:x:{gid_str}:"))
                .unwrap();
            expected_disk_size += gid_str.len() as u64;
        }

        // Add test content.
        rootfs.ensure_dir_with("var/lib/systemd", &db).unwrap();
        touch(&rootfs, "var/lib/systemd/random-seed", 0o770).unwrap();
        rootfs.ensure_dir_with("var/lib/nfs", &db).unwrap();
        touch(&rootfs, "var/lib/nfs/etab", 0o770).unwrap();
        db.mode(0o777);
        rootfs.ensure_dir_with("var/lib/test/nested", &db).unwrap();
        touch(&rootfs, "var/lib/test/nested/file", 0o770).unwrap();
        rootfs
            .symlink("../", "var/lib/test/nested/symlink")
            .unwrap();
        cap_primitives::fs::symlink_contents(
            "/var/lib/foo",
            &rootfs.as_filelike_view(),
            "var/lib/test/absolute-symlink",
        )
        .unwrap();

        // Also make this a sanity test for our directory size API
        let cancellable = gio::Cancellable::new();
        assert_eq!(
            directory_size(rootfs.as_raw_fd(), cancellable.reborrow_cxx()).unwrap(),
            expected_disk_size
        );

        var_to_tmpfiles(&rootfs, gio::Cancellable::NONE).unwrap();

        let autovar_path = "usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf";
        assert!(!rootfs.try_exists("var/lib").unwrap());
        assert!(rootfs.try_exists(autovar_path).unwrap());
        let entries: Vec<String> = rootfs
            .read_to_string(autovar_path)
            .unwrap()
            .lines()
            .map(|s| s.to_owned())
            .collect();
        let expected = &[
            "L /var/lib/test/absolute-symlink - - - - /var/lib/foo",
            "L /var/lib/test/nested/symlink - - - - ../",
            "d /var/lib 0755 test-user test-group - -",
            "d /var/lib/nfs 0755 test-user test-group - -",
            "d /var/lib/systemd 0755 test-user test-group - -",
            "d /var/lib/test 0777 test-user test-group - -",
            "d /var/lib/test/nested 0777 test-user test-group - -",
            "f /var/lib/nfs/etab 0770 test-user test-group - -",
        ];
        assert_eq!(entries, expected, "{:#?}", entries);
    }

    #[test]
    fn test_prepare_symlinks() {
        let rootfs = cap_tempfile::tempdir(cap_std::ambient_authority()).unwrap();
        let mut db = DirBuilder::new();
        db.recursive(true);
        db.mode(0o755);
        rootfs.ensure_dir_with("usr/local", &db).unwrap();
        rootfs.ensure_dir_with("var/lib/alternatives", &db).unwrap();
        rootfs.ensure_dir_with("var/lib/vagrant", &db).unwrap();

        rootfs_prepare_links(rootfs.as_raw_fd(), false).unwrap();
        {
            let usr_dir = rootfs.open_dir("usr").unwrap();
            let local_target = usr_dir.read_link("local").unwrap();
            assert_eq!(local_target.to_str(), Some("../var/usrlocal"));
        }
        {
            let varlib_dir = rootfs.open_dir("var/lib").unwrap();
            let varcases = &[
                ("alternatives", "../../usr/lib/alternatives"),
                ("vagrant", "../../usr/lib/vagrant"),
            ];
            for (linkpath, content) in varcases {
                let target = varlib_dir.read_link(*linkpath);
                assert!(target.is_ok(), "/var/lib/{}", linkpath);
                assert_eq!(target.unwrap().to_str(), Some(*content));
            }
        }
    }

    #[test]
    fn test_tweak_selinux_timestamps() {
        static PREFIX: &str = "usr/etc/selinux/targeted/contexts/files";

        let rootfs = &cap_tempfile::tempdir(cap_std::ambient_authority()).unwrap();
        let mut db = dirbuilder_from_mode(0o755);
        db.recursive(true);
        rootfs
            .ensure_dir_with("usr/etc/selinux/targeted/contexts/files", &db)
            .unwrap();
        let files = &["file_contexts", "file_contexts.homedirs"];
        let perms = cap_std::fs::Permissions::from_mode(0o755);

        let mut metas = vec![];
        for fname in files {
            let binpath = format!("{PREFIX}/{fname}.bin");
            rootfs
                .atomic_write_with_perms(&binpath, "", perms.clone())
                .unwrap();
            let fpath = format!("{PREFIX}/{fname}");
            rootfs
                .atomic_write_with_perms(&fpath, "", perms.clone())
                .unwrap();
            let before_meta = rootfs.symlink_metadata(fpath).unwrap();
            metas.push(before_meta);
        }

        // File timestamps may not increment faster than e.g. 10ms.  Really
        // what we should be doing is using inode versions.
        std::thread::sleep(std::time::Duration::from_millis(100));
        tweak_selinux_timestamps(&rootfs, gio::Cancellable::NONE).unwrap();

        for (fname, before_meta) in files.iter().zip(metas.iter()) {
            let fpath = format!("{}/{}", PREFIX, fname);
            let after = rootfs.symlink_metadata(&fpath).unwrap();
            if before_meta.mtime() == after.mtime() {
                assert_ne!(before_meta.mtime_nsec(), after.mtime_nsec());
            }
        }
    }

    #[test]
    fn test_hardlink_rpmdb_base_location() {
        let rootfs = &cap_tempfile::tempdir(cap_std::ambient_authority()).unwrap();

        {
            let done = hardlink_rpmdb_base_location(&rootfs, gio::Cancellable::NONE).unwrap();
            assert_eq!(done, false);
        }

        let dirs = &[RPMOSTREE_RPMDB_LOCATION, "usr/share/rpm/foo/bar"];
        let mut db = dirbuilder_from_mode(0o755);
        db.recursive(true);
        for entry in dirs {
            rootfs.ensure_dir_with(*entry, &db).unwrap();
        }
        let files = &[
            "usr/share/rpm/rpmdb.sqlite",
            "usr/share/rpm/foo/bar/placeholder",
        ];
        for entry in files {
            rootfs.create(*entry).unwrap();
        }

        let done = hardlink_rpmdb_base_location(&rootfs, gio::Cancellable::NONE).unwrap();
        assert_eq!(done, true);

        assert_eq!(rootfs.try_exists(RPMOSTREE_BASE_RPMDB).unwrap(), true);
        let placeholder = rootfs
            .metadata(format!("{}/foo/bar/placeholder", RPMOSTREE_BASE_RPMDB))
            .unwrap();
        assert_eq!(placeholder.is_file(), true);
        let rpmdb = rootfs
            .metadata(format!("{}/rpmdb.sqlite", RPMOSTREE_BASE_RPMDB))
            .unwrap();
        assert_eq!(rpmdb.is_file(), true);
        let sysimage_link = rootfs.read_link(RPMOSTREE_SYSIMAGE_RPMDB).unwrap();
        assert_eq!(&sysimage_link, Path::new("../../share/rpm"));
    }

    #[test]
    fn test_postprocess_rpm_macro() {
        static MACRO_PATH: &str = "usr/lib/rpm/macros.d/macros.rpm-ostree";
        let expected_content = format!("%_dbpath /{}\n", RPMOSTREE_RPMDB_LOCATION);
        let rootfs = cap_tempfile::tempdir(cap_std::ambient_authority()).unwrap();
        {
            postprocess_rpm_macro(&rootfs).unwrap();

            assert_eq!(rootfs.exists(MACRO_PATH), true);
            let macrofile = rootfs.metadata(MACRO_PATH).unwrap();
            assert_eq!(macrofile.is_file(), true);
            assert_eq!(macrofile.permissions().mode() & 0o777, 0o644);
            let content = rootfs.read_to_string(MACRO_PATH).unwrap();
            assert_eq!(content, expected_content);
        }
        {
            // Re-run, check basic idempotency.
            postprocess_rpm_macro(&rootfs).unwrap();

            let content = rootfs.read_to_string(MACRO_PATH).unwrap();
            assert_eq!(content, expected_content);
        }
    }
}
