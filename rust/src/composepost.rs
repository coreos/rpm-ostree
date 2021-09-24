//! Logic for post-processing a filesystem tree, server-side.
//!
//! This code runs server side to "postprocess" a filesystem tree (usually
//! containing mostly RPMs) in order to prepare it as an OSTree commit.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::bwrap::Bubblewrap;
use crate::cxxrsutil::*;
use crate::ffi::BubblewrapMutability;
use crate::ffiutil::ffi_view_openat_dir;
use crate::normalization;
use crate::passwd::PasswdDB;
use crate::treefile::Treefile;
use crate::{bwrap, importer};
use anyhow::{anyhow, bail, format_err, Context, Result};
use camino::Utf8Path;
use fn_error_context::context;
use gio::prelude::*;
use gio::FileType;
use nix::sys::stat::Mode;
use openat_ext::OpenatDirExt;
use ostree_ext::{gio, glib};
use rayon::prelude::*;
use std::borrow::Cow;
use std::collections::BTreeSet;
use std::convert::TryInto;
use std::fmt::Write as FmtWrite;
use std::io::{BufRead, BufReader, Seek, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::io::AsRawFd;
use std::os::unix::prelude::IntoRawFd;
use std::path::Path;
use std::pin::Pin;
use std::rc::Rc;
use subprocess::{Exec, Redirection};

/* See rpmostree-core.h */
const RPMOSTREE_BASE_RPMDB: &str = "usr/lib/sysimage/rpm-ostree-base-db";
const RPMOSTREE_RPMDB_LOCATION: &str = "usr/share/rpm";
const RPMOSTREE_SYSIMAGE_RPMDB: &str = "usr/lib/sysimage/rpm";
pub(crate) const TRADITIONAL_RPMDB_LOCATION: &str = "var/lib/rpm";

#[context("Moving {}", name)]
fn dir_move_if_exists(src: &openat::Dir, dest: &openat::Dir, name: &str) -> Result<()> {
    if src.exists(name)? {
        openat::rename(src, name, dest, name)?;
    }
    Ok(())
}

/// Initialize an ostree-oriented root filesystem.
///
/// This is hardcoded; in the future we may make more things configurable,
/// but the goal is for all state to be in `/etc` and `/var`.
#[context("Initializing rootfs")]
fn compose_init_rootfs(rootfs_dfd: &openat::Dir, tmp_is_dir: bool) -> Result<()> {
    use nix::fcntl::OFlag;
    println!("Initializing rootfs");

    let default_dirmode = Mode::from_bits(0o755).unwrap();

    // Unfortunately fchmod() doesn't operate on an O_PATH descriptor
    let flag = OFlag::O_DIRECTORY | OFlag::O_CLOEXEC;
    let nix_rootfs = nix::dir::Dir::openat(rootfs_dfd.as_raw_fd(), ".", flag, default_dirmode)?;

    const TOPLEVEL_DIRS: &[&str] = &["dev", "proc", "run", "sys", "var", "sysroot"];
    const TOPLEVEL_SYMLINKS: &[(&str, &str)] = &[
        ("var/opt", "opt"),
        ("var/srv", "srv"),
        ("var/mnt", "mnt"),
        ("var/roothome", "root"),
        ("var/home", "home"),
        ("run/media", "media"),
        ("sysroot/ostree", "ostree"),
    ];

    nix::sys::stat::fchmod(nix_rootfs.as_raw_fd(), default_dirmode).context("rootfs chmod")?;

    TOPLEVEL_DIRS
        .par_iter()
        .try_for_each(|&d| rootfs_dfd.ensure_dir(d, default_dirmode.bits()))?;
    TOPLEVEL_SYMLINKS
        .par_iter()
        .try_for_each(|&(dest, src)| rootfs_dfd.symlink(src, dest))?;

    if tmp_is_dir {
        let tmp_mode = 0o1777;
        rootfs_dfd.ensure_dir("tmp", tmp_mode)?;
        nix::sys::stat::fchmodat(
            Some(nix_rootfs.as_raw_fd()),
            "tmp",
            Mode::from_bits(tmp_mode).unwrap(),
            nix::sys::stat::FchmodatFlags::FollowSymlink,
        )?;
    } else {
        rootfs_dfd.symlink("tmp", "sysroot/tmp")?;
    }

    Ok(())
}

/// Prepare rootfs for commit.
///
/// Initialize a basic root filesystem in @target_root_dfd, then walk over the
/// root filesystem in @src_rootfs_fd and take the basic content (/usr, /boot, /var)
/// and cherry pick only specific bits of the rest of the toplevel like compatibility
/// symlinks (e.g. /lib64 -> /usr/lib64) if they exist.
#[context("Preparing rootfs for commit")]
pub fn compose_prepare_rootfs(
    src_rootfs_dfd: i32,
    target_rootfs_dfd: i32,
    treefile: &mut Treefile,
) -> CxxResult<()> {
    let src_rootfs_dfd = &crate::ffiutil::ffi_view_openat_dir(src_rootfs_dfd);
    let target_rootfs_dfd = &crate::ffiutil::ffi_view_openat_dir(target_rootfs_dfd);

    let tmp_is_dir = treefile.parsed.tmp_is_dir.unwrap_or_default();
    compose_init_rootfs(target_rootfs_dfd, tmp_is_dir)?;

    println!("Moving /usr to target");
    openat::rename(src_rootfs_dfd, "usr", target_rootfs_dfd, "usr")?;
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
fn postprocess_useradd(rootfs_dfd: &openat::Dir) -> Result<()> {
    let path = Path::new("usr/etc/default/useradd");
    if let Some(f) = rootfs_dfd.open_file_optional(path)? {
        rootfs_dfd.write_file_with(&path, 0o644, |bufw| -> Result<_> {
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
        })?;
    }
    Ok(())
}

// We keep hitting issues with the ostree-remount preset not being
// enabled; let's just do this rather than trying to propagate the
// preset everywhere.
fn postprocess_presets(rootfs_dfd: &openat::Dir) -> Result<()> {
    let wantsdir = "usr/lib/systemd/system/multi-user.target.wants";
    rootfs_dfd.ensure_dir_all(wantsdir, 0o755)?;
    for service in &["ostree-remount.service", "ostree-finalize-staged.path"] {
        let target = format!("../{}", service);
        let loc = Path::new(wantsdir).join(service);
        rootfs_dfd.symlink(&loc, target)?;
    }
    Ok(())
}

/// Write an RPM macro file to ensure the rpmdb path is set on the client side.
pub fn compose_postprocess_rpm_macro(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs = &crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);
    postprocess_rpm_macro(rootfs)?;
    Ok(())
}

/// Ensure our own `_dbpath` macro exists in the tree.
#[context("Writing _dbpath RPM macro")]
fn postprocess_rpm_macro(rootfs_dfd: &openat::Dir) -> Result<()> {
    static RPM_MACROS_DIR: &str = "usr/lib/rpm/macros.d";
    static MACRO_FILENAME: &str = "macros.rpm-ostree";
    rootfs_dfd.ensure_dir_all(RPM_MACROS_DIR, 0o755)?;
    let rpm_macros_dfd = rootfs_dfd.sub_dir(RPM_MACROS_DIR)?;
    rpm_macros_dfd.write_file_with(&MACRO_FILENAME, 0o644, |w| -> Result<()> {
        w.write_all(b"%_dbpath /")?;
        w.write_all(RPMOSTREE_RPMDB_LOCATION.as_bytes())?;
        w.write_all(b"\n")?;
        Ok(())
    })?;
    rpm_macros_dfd.set_mode(MACRO_FILENAME, 0o644)?;
    Ok(())
}

// This function does two things: (1) make sure there is a /home --> /var/home substitution rule,
// and (2) make sure there *isn't* a /var/home -> /home substition rule. The latter check won't
// technically be needed once downstreams have:
// https://src.fedoraproject.org/rpms/selinux-policy/pull-request/14
fn postprocess_subs_dist(rootfs_dfd: &openat::Dir) -> Result<()> {
    let path = Path::new("usr/etc/selinux/targeted/contexts/files/file_contexts.subs_dist");
    if let Some(f) = rootfs_dfd.open_file_optional(path)? {
        rootfs_dfd.write_file_with(&path, 0o644, |w| -> Result<()> {
            let f = BufReader::new(&f);
            for line in f.lines() {
                let line = line?;
                if line.starts_with("/var/home ") {
                    w.write_all(b"# https://github.com/projectatomic/rpm-ostree/pull/1754\n")?;
                    w.write_all(b"# ")?;
                }
                w.write_all(line.as_bytes())?;
                w.write_all(b"\n")?;
            }
            w.write_all(b"# https://github.com/projectatomic/rpm-ostree/pull/1754\n")?;
            w.write_all(b"/home /var/home")?;
            w.write_all(b"\n")?;
            Ok(())
        })?;
    }
    Ok(())
}

/// Final processing steps.
///
/// This function is called from rpmostree_postprocess_final(); think of
/// it as the bits of that function that we've chosen to implement in Rust.
/// It takes care of all things that are really required to use rpm-ostree
/// on the target host.
pub fn compose_postprocess_final(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs_dfd = crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let tasks = [
        postprocess_useradd,
        postprocess_presets,
        postprocess_subs_dist,
        postprocess_rpm_macro,
    ];
    Ok(tasks.par_iter().try_for_each(|f| f(&rootfs_dfd))?)
}

#[context("Handling treefile 'units'")]
fn compose_postprocess_units(rootfs_dfd: &openat::Dir, treefile: &mut Treefile) -> Result<()> {
    let units = if let Some(u) = treefile.parsed.units.as_ref() {
        u
    } else {
        return Ok(());
    };
    let multiuser_wants = Path::new("usr/etc/systemd/system/multi-user.target.wants");
    // Sanity check
    if !rootfs_dfd.exists("usr/etc")? {
        return Err(anyhow!("Missing usr/etc in rootfs"));
    }
    rootfs_dfd.ensure_dir_all(multiuser_wants, 0o755)?;

    for unit in units {
        let dest = multiuser_wants.join(unit);
        if rootfs_dfd.exists(&dest)? {
            continue;
        }

        println!("Adding {} to multi-user.target.wants", unit);
        let target = format!("/usr/lib/systemd/system/{}", unit);
        rootfs_dfd.symlink(&dest, &target)?;
    }
    Ok(())
}

#[context("Handling treefile 'default-target'")]
fn compose_postprocess_default_target(rootfs_dfd: &openat::Dir, target: &str) -> Result<()> {
    /* This used to be in /etc, but doing it in /usr makes more sense, as it's
     * part of the OS defaults. This was changed in particular to work with
     * ConditionFirstBoot= which runs `systemctl preset-all`:
     * https://github.com/projectatomic/rpm-ostree/pull/1425
     */
    let default_target_path = "usr/lib/systemd/system/default.target";
    rootfs_dfd.remove_file_optional(default_target_path)?;
    let dest = format!("/usr/lib/systemd/system/{}", target);
    rootfs_dfd.symlink(default_target_path, dest)?;

    Ok(())
}

/// The treefile format has two kinds of postprocessing scripts;
/// there's a single `postprocess-script` as well as inline (anonymous)
/// scripts.  This function executes both kinds in bwrap containers.
fn compose_postprocess_scripts(
    rootfs_dfd: &openat::Dir,
    treefile: &mut Treefile,
    unified_core: bool,
) -> Result<()> {
    // Execute the anonymous (inline) scripts.
    for (i, script) in treefile.parsed.postprocess.iter().flatten().enumerate() {
        let binpath = format!("/usr/bin/rpmostree-postprocess-inline-{}", i);
        let target_binpath = &binpath[1..];

        rootfs_dfd.write_file_contents(target_binpath, 0o755, script)?;
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
        rootfs_dfd.write_file_with(target_binpath, 0o755, |w| std::io::copy(&mut reader, w))?;
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
    rootfs_dfd: &openat::Dir,
    treefile: &mut Treefile,
) -> CxxResult<()> {
    for name in treefile.parsed.remove_files.iter().flatten() {
        let p = Path::new(name);
        if p.is_absolute() {
            return Err(anyhow!("Invalid absolute path: {}", name).into());
        }
        if name.contains("..") {
            return Err(anyhow!("Invalid '..' in path: {}", name).into());
        }
        println!("Deleting: {}", name);
        rootfs_dfd.remove_all(name)?;
    }
    Ok(())
}

fn compose_postprocess_add_files(rootfs_dfd: &openat::Dir, treefile: &mut Treefile) -> Result<()> {
    // Make a deep copy here because get_add_file_fd() also wants an &mut
    // reference.
    let add_files: Vec<_> = treefile
        .parsed
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
            rootfs_dfd.ensure_dir_all(parent, 0o755)?;
        }

        let fd = treefile.get_add_file(&src);
        fd.seek(std::io::SeekFrom::Start(0))?;
        let mut reader = std::io::BufReader::new(fd);
        let mode = reader.get_mut().metadata()?.permissions().mode();
        rootfs_dfd.write_file_with(dest, mode, |w| std::io::copy(&mut reader, w))?;
    }
    Ok(())
}

#[context("Symlinking {}", TRADITIONAL_RPMDB_LOCATION)]
fn compose_postprocess_rpmdb(rootfs_dfd: &openat::Dir) -> Result<()> {
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
    rootfs_dfd.remove_all(TRADITIONAL_RPMDB_LOCATION)?;
    rootfs_dfd.symlink(
        TRADITIONAL_RPMDB_LOCATION,
        format!("../../{}", RPMOSTREE_RPMDB_LOCATION),
    )?;
    Ok(())
}

/// Rust portion of rpmostree_treefile_postprocessing()
pub fn compose_postprocess(
    rootfs_dfd: i32,
    treefile: &mut Treefile,
    next_version: &str,
    unified_core: bool,
) -> CxxResult<()> {
    let rootfs_dfd = &crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);

    // One of several dances we do around this that really needs to be completely
    // reworked.
    if rootfs_dfd.exists("etc")? {
        rootfs_dfd.local_rename("etc", "usr/etc")?;
    }

    compose_postprocess_rpmdb(rootfs_dfd)?;
    compose_postprocess_units(rootfs_dfd, treefile)?;
    if let Some(t) = treefile.parsed.default_target.as_deref() {
        compose_postprocess_default_target(rootfs_dfd, t)?;
    }

    treefile.write_compose_json(rootfs_dfd)?;

    let etc_guard = crate::core::prepare_tempetc_guard(rootfs_dfd.as_raw_fd())?;
    // These ones depend on the /etc path
    compose_postprocess_mutate_os_release(rootfs_dfd, treefile, next_version)?;
    compose_postprocess_remove_files(rootfs_dfd, treefile)?;
    compose_postprocess_add_files(rootfs_dfd, treefile)?;
    etc_guard.undo()?;

    compose_postprocess_scripts(rootfs_dfd, treefile, unified_core)?;

    Ok(())
}

/// Implementation of the treefile `mutate-os-release` field.
#[context("Updating os-release with commit version")]
fn compose_postprocess_mutate_os_release(
    rootfs_dfd: &openat::Dir,
    treefile: &mut Treefile,
    next_version: &str,
) -> Result<()> {
    let base_version = if let Some(base_version) = treefile.parsed.mutate_os_release.as_deref() {
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
        rootfs_dfd,
        crate::ffi::BubblewrapMutability::Immutable,
    )?;
    bwrap.append_child_argv(&["realpath", "/etc/os-release"]);
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
    let contents = rootfs_dfd
        .read_to_string(path)
        .with_context(|| format!("Reading {}", path))?;
    let new_contents = mutate_os_release_contents(&contents, base_version, next_version);
    rootfs_dfd
        .write_file_contents(path, 0o644, new_contents.as_bytes())
        .with_context(|| format!("Writing {}", path))?;
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
    let rootfs_dfd = &crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let path = "usr/etc/nsswitch.conf";
    let nsswitch = rootfs_dfd.read_to_string(path)?;
    let nsswitch = add_altfiles(&nsswitch)?;
    rootfs_dfd.write_file_contents(path, 0o644, nsswitch.as_bytes())?;
    Ok(())
}

pub fn convert_var_to_tmpfiles_d(
    rootfs_dfd: i32,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
) -> CxxResult<()> {
    let rootfs = crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let cancellable = &cancellable.gobj_wrap();

    // TODO(lucab): unify this logic with the one in rpmostree-importer.cxx.
    var_to_tmpfiles(&rootfs, Some(cancellable))?;
    Ok(())
}

#[context("Converting /var to tmpfiles.d")]
fn var_to_tmpfiles(rootfs: &openat::Dir, cancellable: Option<&gio::Cancellable>) -> Result<()> {
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
    rootfs
        .remove_all("var/run")
        .context("Failed to remove /var/run")?;

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
    rootfs.ensure_dir_all("usr/lib/tmpfiles.d", 0o755)?;
    rootfs.write_file_with_sync(
        "usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf",
        0o644,
        |bufwr| -> Result<()> {
            let mut prefix = "var".to_string();
            let mut entries = BTreeSet::new();
            convert_path_to_tmpfiles_d_recurse(
                &mut entries,
                &pwdb,
                rootfs,
                &mut prefix,
                &cancellable,
            )
            .with_context(|| format!("Analyzing /{} content", prefix))?;
            for line in entries {
                bufwr.write_all(line.as_bytes())?;
                writeln!(bufwr)?;
            }
            Ok(())
        },
    )?;

    Ok(())
}

/// Recursively explore target directory and translate content to tmpfiles.d entries.
///
/// This proceeds depth-first and progressively deletes translated subpaths as it goes.
/// `prefix` is updated at each recursive step, so that in case of errors it can be
/// used to pinpoint the faulty path.
fn convert_path_to_tmpfiles_d_recurse(
    out_entries: &mut BTreeSet<String>,
    pwdb: &PasswdDB,
    rootfs: &openat::Dir,
    prefix: &mut String,
    cancellable: &Option<&gio::Cancellable>,
) -> Result<()> {
    use openat::SimpleType;

    let current_prefix = prefix.clone();
    for subpath in rootfs.list_dir(&current_prefix)? {
        if cancellable.map(|c| c.is_cancelled()).unwrap_or_default() {
            bail!("Cancelled");
        };

        let subpath = subpath?;
        let fname: &Utf8Path = Path::new(subpath.file_name()).try_into()?;
        let full_path = format!("{}/{}", &current_prefix, fname);
        let path_type = subpath.simple_type().unwrap_or(SimpleType::Other);

        // Workaround for nfs-utils in RHEL7:
        // https://bugzilla.redhat.com/show_bug.cgi?id=1427537
        let mut retain_entry = false;
        if path_type == SimpleType::File && full_path.starts_with("var/lib/nfs") {
            retain_entry = true;
        }

        if !retain_entry && !matches!(path_type, SimpleType::Dir | SimpleType::Symlink) {
            rootfs.remove_file_optional(&full_path)?;
            println!("Ignoring non-directory/non-symlink '{}'", &full_path);
            continue;
        }

        // Translate this file entry.
        let entry = {
            let meta = rootfs.metadata(&full_path)?;
            let mode = meta.stat().st_mode & !libc::S_IFMT;

            let file_info = gio::FileInfo::new();
            file_info.set_attribute_uint32("unix::mode", mode);

            match path_type {
                SimpleType::Dir => file_info.set_file_type(FileType::Directory),
                SimpleType::Symlink => {
                    file_info.set_file_type(FileType::SymbolicLink);
                    let link_target = rootfs.read_link(&full_path)?;
                    let target_path = Utf8Path::from_path(&link_target).ok_or_else(|| {
                        format_err!("non UTF-8 symlink target '{}'", &link_target.display())
                    })?;
                    file_info.set_symlink_target(target_path.as_str());
                }
                SimpleType::File => file_info.set_file_type(FileType::Regular),
                x => unreachable!("invalid path type: {:?}", x),
            };

            let abs_path = format!("/{}", full_path);
            let username = pwdb.lookup_user(meta.stat().st_uid)?;
            let groupname = pwdb.lookup_group(meta.stat().st_gid)?;
            importer::translate_to_tmpfiles_d(&abs_path, &file_info, &username, &groupname)?
        };
        out_entries.insert(entry);

        if path_type == SimpleType::Dir {
            // New subdirectory discovered, recurse into it.
            *prefix = full_path.clone();
            convert_path_to_tmpfiles_d_recurse(out_entries, pwdb, rootfs, prefix, cancellable)?;
        }

        rootfs.remove_all(&full_path)?;
    }
    Ok(())
}

/// Walk over the root filesystem and perform some core conversions
/// from RPM conventions to OSTree conventions.
///
/// For example:
///  - Symlink /usr/local -> /var/usrlocal
///  - Symlink /var/lib/alternatives -> /usr/lib/alternatives
///  - Symlink /var/lib/vagrant -> /usr/lib/vagrant
#[context("Preparing symlinks in rootfs")]
pub fn rootfs_prepare_links(rootfs_dfd: i32) -> CxxResult<()> {
    let rootfs = crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);

    rootfs
        .remove_all("usr/local")
        .context("Removing /usr/local")?;
    let state_paths = &["usr/lib/alternatives", "usr/lib/vagrant"];
    for entry in state_paths {
        rootfs
            .ensure_dir_all(*entry, 0o0755)
            .with_context(|| format!("Creating '/{}'", entry))?;
    }

    let symlinks = &[
        ("../var/usrlocal", "usr/local"),
        ("../../usr/lib/alternatives", "var/lib/alternatives"),
        ("../../usr/lib/vagrant", "var/lib/vagrant"),
    ];
    for (target, linkpath) in symlinks {
        ensure_symlink(&rootfs, target, linkpath)?;
    }

    Ok(())
}

/// Create a symlink at `linkpath` if it does not exist, pointing to `target`.
///
/// This is idempotent and does not alter any content already existing at `linkpath`.
/// It returns `true` if the symlink has been created, `false` otherwise.
#[context("Symlinking '/{}' to empty directory '/{}'", linkpath, target)]
fn ensure_symlink(rootfs: &openat::Dir, target: &str, linkpath: &str) -> Result<bool> {
    use openat::SimpleType;

    if let Some(meta) = rootfs.metadata_optional(linkpath)? {
        match meta.simple_type() {
            SimpleType::Symlink => {
                // We assume linkpath already points to the correct target,
                // thus this short-circuits in an idempotent way.
                return Ok(false);
            }
            SimpleType::Dir => rootfs.remove_dir(linkpath)?,
            _ => bail!("Content already exists at link path"),
        };
    } else {
        // For maximum compatibility, create parent directories too.  This
        // is necessary when we're doing layering on top of a base commit,
        // and the /var will be empty.  We should probably consider running
        // systemd-tmpfiles to setup the temporary /var.
        if let Some(parent) = Path::new(linkpath).parent() {
            rootfs.ensure_dir_all(parent, 0o755)?;
        }
    }

    rootfs.symlink(linkpath, target)?;
    Ok(true)
}

pub fn workaround_selinux_cross_labeling(
    rootfs_dfd: i32,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
) -> CxxResult<()> {
    let rootfs = crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let cancellable = &cancellable.gobj_wrap();

    tweak_selinux_timestamps(&rootfs, Some(cancellable))?;
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
fn tweak_selinux_timestamps(
    rootfs: &openat::Dir,
    cancellable: Option<&gio::Cancellable>,
) -> Result<()> {
    // Handle the policy being in both /usr/etc and /etc since
    // this function can be called at different points.
    let policy_path = if rootfs.exists("usr/etc")? {
        "usr/etc/selinux"
    } else {
        "etc/selinux"
    };

    if rootfs.exists(policy_path)? {
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
    rootfs: &openat::Dir,
    prefix: &mut String,
    cancellable: &Option<&gio::Cancellable>,
) -> Result<()> {
    use openat::SimpleType;

    let current_prefix = prefix.clone();
    for subpath in rootfs.list_dir(&current_prefix)? {
        if cancellable.map(|c| c.is_cancelled()).unwrap_or_default() {
            bail!("Cancelled");
        };

        let subpath = subpath?;
        let full_path = {
            let fname = subpath.file_name();
            let path_name = fname
                .to_str()
                .ok_or_else(|| anyhow!("invalid non-UTF-8 path: {:?}", fname))?;
            format!("{}/{}", &current_prefix, &path_name)
        };
        let path_type = subpath.simple_type().unwrap_or(SimpleType::Other);

        if path_type == SimpleType::Dir {
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

pub fn prepare_rpmdb_base_location(
    rootfs_dfd: i32,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
) -> CxxResult<()> {
    let rootfs = crate::ffiutil::ffi_view_openat_dir(rootfs_dfd);
    let cancellable = &cancellable.gobj_wrap();

    hardlink_rpmdb_base_location(&rootfs, Some(cancellable))?;
    Ok(())
}

/// Recurse into this directory and return the total size of all regular files.
#[context("Computing directory size")]
pub fn directory_size(
    dfd: i32,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
) -> CxxResult<u64> {
    let cancellable = &cancellable.gobj_wrap();
    let dfd = crate::ffiutil::ffi_view_openat_dir(dfd);
    fn directory_size_recurse(d: &openat::Dir, cancellable: &gio::Cancellable) -> Result<u64> {
        let mut r = 0;
        for ent in d.list_dir(".")? {
            cancellable.set_error_if_cancelled()?;
            let ent = ent?;
            let meta = d
                .metadata(ent.file_name())
                .with_context(|| format!("Failed to access {:?}", ent.file_name()))?;
            match meta.simple_type() {
                openat::SimpleType::Dir => {
                    let child = d.sub_dir(ent.file_name())?;
                    r += directory_size_recurse(&child, cancellable)?;
                }
                openat::SimpleType::File => {
                    r += meta.stat().st_size as u64;
                }
                _ => {}
            }
        }
        Ok(r)
    }
    Ok(directory_size_recurse(&dfd, cancellable)?)
}

#[context("Hardlinking rpmdb to base location")]
fn hardlink_rpmdb_base_location(
    rootfs: &openat::Dir,
    cancellable: Option<&gio::Cancellable>,
) -> Result<bool> {
    if !rootfs.exists(RPMOSTREE_RPMDB_LOCATION)? {
        return Ok(false);
    }

    // Hardlink our own `/usr/lib/sysimage/rpm-ostree-base-db/` hierarchy
    // to the well-known `/usr/share/rpm/`.
    rootfs.ensure_dir_all(RPMOSTREE_BASE_RPMDB, 0o755)?;
    rootfs.set_mode(RPMOSTREE_BASE_RPMDB, 0o755)?;
    hardlink_hierarchy(
        rootfs,
        RPMOSTREE_RPMDB_LOCATION,
        RPMOSTREE_BASE_RPMDB,
        cancellable,
    )?;

    // And write a symlink from the proposed standard /usr/lib/sysimage/rpm
    // to our /usr/share/rpm - eventually we will invert this.
    rootfs.symlink(RPMOSTREE_SYSIMAGE_RPMDB, "../../share/rpm")?;

    Ok(true)
}

#[context("Rewriting rpmdb for target native format")]
fn rewrite_rpmdb_for_target_inner(rootfs_dfd: &openat::Dir, normalize: bool) -> Result<()> {
    let tempetc = crate::core::prepare_tempetc_guard(rootfs_dfd.as_raw_fd())?;

    let dbfd = Rc::new(
        memfd::MemfdOptions::default()
            .allow_sealing(true)
            .create("rpmdb")?
            .into_file(),
    );

    let dbpath_arg = format!("--dbpath=/proc/self/cwd/{}", RPMOSTREE_RPMDB_LOCATION);
    // Fork rpmdb from the *host* rootfs to read the rpmdb back into memory
    let r = Exec::cmd("rpmdb")
        .args(&[dbpath_arg.as_str(), "--exportdb"])
        .cwd(format!("/proc/self/fd/{}", rootfs_dfd.as_raw_fd()))
        .stdout(Redirection::RcFile(Rc::clone(&dbfd)))
        .join()?;
    if !r.success() {
        return Err(anyhow!("Failed to execute rpmdb --exportdb: {:?}", r));
    }

    // Clear out the db on disk
    rootfs_dfd.remove_all(RPMOSTREE_RPMDB_LOCATION)?;
    rootfs_dfd.create_dir(RPMOSTREE_RPMDB_LOCATION, 0o755)?;

    // Only one owner now
    let mut dbfd = Rc::try_unwrap(dbfd).unwrap();
    dbfd.seek(std::io::SeekFrom::Start(0))?;

    // In the interests of build stability, rewrite the INSTALLTIME and INSTALLTID tags
    // to be deterministic and dervied from `SOURCE_DATE_EPOCH` if requested.
    if normalize {
        normalization::rewrite_rpmdb_timestamps(&mut dbfd)?;
    }

    // Fork the target rpmdb to write the content from memory to disk
    let mut bwrap = Bubblewrap::new_with_mutability(rootfs_dfd, BubblewrapMutability::RoFiles)?;
    bwrap.append_child_argv(&["rpmdb", dbpath_arg.as_str(), "--importdb"]);
    bwrap.take_stdin_fd(dbfd.into_raw_fd());
    let cancellable = gio::Cancellable::new();
    bwrap
        .run(cancellable.gobj_rewrap())
        .context("Failed to run rpmdb --importdb")?;

    tempetc.undo()?;

    Ok(())
}

pub(crate) fn rewrite_rpmdb_for_target(rootfs_dfd: i32, normalize: bool) -> CxxResult<()> {
    Ok(rewrite_rpmdb_for_target_inner(
        &ffi_view_openat_dir(rootfs_dfd),
        normalize,
    )?)
}

/// Recursively hard-link `source` hierarchy to `target` directory.
///
/// Both directories must exist beforehand.
#[context("Hardlinking /{} to /{}", source, target)]
fn hardlink_hierarchy(
    rootfs: &openat::Dir,
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
    rootfs: &openat::Dir,
    source_prefix: &str,
    dest_prefix: &str,
    relative_path: &mut String,
    cancellable: &Option<&gio::Cancellable>,
) -> Result<()> {
    use openat::SimpleType;

    let current_dir = relative_path.clone();
    let current_source_dir = format!("{}/{}", source_prefix, relative_path);
    for subpath in rootfs.list_dir(&current_source_dir)? {
        if cancellable.map(|c| c.is_cancelled()).unwrap_or_default() {
            bail!("Cancelled");
        };

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
        let path_type = subpath.simple_type().unwrap_or(SimpleType::Other);

        if path_type == SimpleType::Dir {
            // New subdirectory discovered, create it at the target.
            let perms = rootfs.metadata(&source_path)?.stat().st_mode & !libc::S_IFMT;
            rootfs.ensure_dir(&dest_path, perms)?;
            rootfs.set_mode(&dest_path, perms)?;

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
            openat::hardlink(rootfs, source_path, rootfs, dest_path)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

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

    #[test]
    fn test_init_rootfs() -> Result<()> {
        {
            let t = tempfile::tempdir()?;
            let rootfs = &openat::Dir::open(t.path())?;
            compose_init_rootfs(rootfs, false)?;
            let target = rootfs.read_link("tmp").unwrap();
            assert_eq!(target, Path::new("sysroot/tmp"));
        }
        {
            let t = tempfile::tempdir()?;
            let rootfs = &openat::Dir::open(t.path())?;
            compose_init_rootfs(rootfs, true)?;
            let tmpdir_meta = rootfs.metadata("tmp").unwrap();
            assert!(tmpdir_meta.is_dir());
            assert_eq!(tmpdir_meta.stat().st_mode & 0o7777, 0o1777);
        }
        Ok(())
    }

    #[test]
    fn test_tmpfiles_d_translation() {
        use nix::sys::stat::{umask, Mode};
        use nix::unistd::{getegid, geteuid};

        // Prepare a minimal rootfs as playground.
        umask(Mode::empty());
        let temp_rootfs = tempfile::tempdir().unwrap();
        let rootfs = openat::Dir::open(temp_rootfs.path()).unwrap();
        {
            for dirpath in &["usr/lib", "usr/etc", "var"] {
                rootfs.ensure_dir_all(*dirpath, 0o755).unwrap();
            }
            for filepath in &["usr/lib/passwd", "usr/lib/group"] {
                rootfs.new_file(*filepath, 0o755).unwrap();
            }
            rootfs
                .write_file_contents(
                    "usr/etc/passwd",
                    0o755,
                    format!("test-user:x:{}:{}:::", geteuid(), getegid()),
                )
                .unwrap();
            rootfs
                .write_file_contents(
                    "usr/etc/group",
                    0o755,
                    format!("test-group:x:{}:", getegid()),
                )
                .unwrap();
        }

        // Add test content.
        rootfs.ensure_dir_all("var/lib/systemd", 0o755).unwrap();
        rootfs
            .new_file("var/lib/systemd/random-seed", 0o755)
            .unwrap();
        rootfs.ensure_dir_all("var/lib/nfs", 0o755).unwrap();
        rootfs.new_file("var/lib/nfs/etab", 0o770).unwrap();
        rootfs.ensure_dir_all("var/lib/test/nested", 0o777).unwrap();
        rootfs.new_file("var/lib/test/nested/file", 0o755).unwrap();
        rootfs
            .symlink("var/lib/test/nested/symlink", "../")
            .unwrap();

        // Also make this a sanity test for our directory size API
        let cancellable = gio::Cancellable::new();
        assert_eq!(
            directory_size(rootfs.as_raw_fd(), cancellable.gobj_rewrap()).unwrap(),
            42
        );

        var_to_tmpfiles(&rootfs, gio::NONE_CANCELLABLE).unwrap();

        let autovar_path = "usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf";
        assert!(!rootfs.exists("var/lib").unwrap());
        assert!(rootfs.exists(autovar_path).unwrap());
        let entries: Vec<String> = rootfs
            .read_to_string(autovar_path)
            .unwrap()
            .lines()
            .map(|s| s.to_owned())
            .collect();
        let expected = &[
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
        let temp_rootfs = tempfile::tempdir().unwrap();
        let rootfs = openat::Dir::open(temp_rootfs.path()).unwrap();
        rootfs.ensure_dir_all("usr/local", 0o755).unwrap();

        rootfs_prepare_links(rootfs.as_raw_fd()).unwrap();
        {
            let usr_dir = rootfs.sub_dir("usr").unwrap();
            let local_target = usr_dir.read_link("local").unwrap();
            assert_eq!(local_target.to_str(), Some("../var/usrlocal"));
        }
        {
            let varlib_dir = rootfs.sub_dir("var/lib").unwrap();
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

        let temp_rootfs = tempfile::tempdir().unwrap();
        let rootfs = openat::Dir::open(temp_rootfs.path()).unwrap();
        rootfs
            .ensure_dir_all("usr/etc/selinux/targeted/contexts/files", 0o755)
            .unwrap();
        let files = &["file_contexts", "file_contexts.homedirs"];

        let mut metas = vec![];
        for fname in files {
            let binpath = format!("{}/{}.bin", PREFIX, fname);
            rootfs.new_file(&binpath, 0o755).unwrap();
            let fpath = format!("{}/{}", PREFIX, fname);
            rootfs.new_file(&fpath, 0o755).unwrap();
            let before_meta = rootfs.metadata(fpath).unwrap();
            metas.push(before_meta);
        }

        // File timestamps may not increment faster than e.g. 10ms.  Really
        // what we should be doing is using inode versions.
        std::thread::sleep(std::time::Duration::from_millis(100));
        tweak_selinux_timestamps(&rootfs, gio::NONE_CANCELLABLE).unwrap();

        for (fname, before_meta) in files.iter().zip(metas.iter()) {
            let fpath = format!("{}/{}", PREFIX, fname);
            let after = rootfs.metadata(&fpath).unwrap();
            if before_meta.stat().st_mtime == after.stat().st_mtime {
                assert_ne!(before_meta.stat().st_mtime_nsec, after.stat().st_mtime_nsec);
            }
        }
    }

    #[test]
    fn test_hardlink_rpmdb_base_location() {
        let temp_rootfs = tempfile::tempdir().unwrap();
        let rootfs = openat::Dir::open(temp_rootfs.path()).unwrap();

        {
            let done = hardlink_rpmdb_base_location(&rootfs, gio::NONE_CANCELLABLE).unwrap();
            assert_eq!(done, false);
        }

        let dirs = &[RPMOSTREE_RPMDB_LOCATION, "usr/share/rpm/foo/bar"];
        for entry in dirs {
            rootfs.ensure_dir_all(*entry, 0o755).unwrap();
        }
        let files = &[
            "usr/share/rpm/rpmdb.sqlite",
            "usr/share/rpm/foo/bar/placeholder",
        ];
        for entry in files {
            rootfs.write_file(*entry, 0o755).unwrap();
        }

        let done = hardlink_rpmdb_base_location(&rootfs, gio::NONE_CANCELLABLE).unwrap();
        assert_eq!(done, true);

        assert_eq!(rootfs.exists(RPMOSTREE_BASE_RPMDB).unwrap(), true);
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
        let temp_rootfs = tempfile::tempdir().unwrap();
        let rootfs = openat::Dir::open(temp_rootfs.path()).unwrap();
        {
            postprocess_rpm_macro(&rootfs).unwrap();

            assert_eq!(rootfs.exists(MACRO_PATH).unwrap(), true);
            let macrofile = rootfs.metadata(MACRO_PATH).unwrap();
            assert_eq!(macrofile.is_file(), true);
            assert_eq!(macrofile.stat().st_mode & 0o777, 0o644);
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
