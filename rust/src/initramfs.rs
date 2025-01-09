//! Generate an "overlay" initramfs image
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use anyhow::{anyhow, Context, Result};
use camino::Utf8Path;
use cap_std::fs::Dir;
use cap_std::io_lifetimes::AsFilelike;
use cap_std_ext::cap_std;
use cap_std_ext::prelude::CapStdExtCommandExt;
use fn_error_context::context;
use ostree_ext::{gio, glib, prelude::*};
use rustix::fd::BorrowedFd;
use std::collections::BTreeSet;
use std::collections::HashSet;
use std::io::prelude::*;
use std::os::fd::AsRawFd as _;
use std::os::unix::io::IntoRawFd;
use std::path::{Component, Path, PathBuf};
use std::pin::Pin;
use std::process::Command;
use std::{fs, io};

fn list_files_recurse<P: glib::IsA<gio::Cancellable>>(
    d: &cap_std::fs::Dir,
    path: &str,
    filelist: &mut BTreeSet<String>,
    cancellable: Option<&P>,
) -> Result<()> {
    // Maybe add a glib feature for openat-ext to support cancellability?  Or
    // perhaps we should just use futures and ensure GCancellable bridges to that.
    if let Some(c) = cancellable {
        c.set_error_if_cancelled()?;
    }
    let meta = d.symlink_metadata(path).context("stat")?;
    if meta.is_dir() {
        for e in d.read_dir(path).context("readdir")? {
            let e = e.context("readdir")?;
            let name = e.file_name();
            let name: &Utf8Path = Path::new(&name).try_into()?;
            let subpath = format!("{}/{}", path, name);
            list_files_recurse(d, &subpath, filelist, cancellable)?;
        }
    } else if !(meta.is_file() || meta.is_symlink()) {
        anyhow::bail!("Invalid non-regfile/symlink/directory: {}", path);
    }
    if !filelist.contains(path) {
        filelist.insert(path.to_string());
    }
    Ok(())
}

fn gather_filelist<P: glib::IsA<gio::Cancellable>>(
    d: &cap_std::fs::Dir,
    input: &HashSet<String>,
    cancellable: Option<&P>,
) -> Result<BTreeSet<String>> {
    let mut filelist: BTreeSet<String> = BTreeSet::new();
    for file in input {
        let file = match file.strip_prefix("/etc/") {
            Some(f) => f,
            None => anyhow::bail!("Invalid non-/etc path: {}", file),
        };

        // We need to include all the directories leading up to the path because cpio doesn't do
        // that, and the kernel at extraction time won't create missing leading dirs. But this also
        // serves as validation that the path is canonical and doesn't do e.g. /etc/foo/../bar.
        let mut path = PathBuf::new();
        for component in Path::new(file).components() {
            match component {
                Component::Normal(c) => {
                    path.push(c);
                    // safe to unwrap here since it's all built up of components of a known UTF-8 path
                    let s = path.to_str().unwrap();
                    if !filelist.contains(s) {
                        filelist.insert(s.into());
                    }
                }
                _ => anyhow::bail!("invalid path /etc/{}: must be canonical", file),
            }
        }

        list_files_recurse(d, file, &mut filelist, cancellable)
            .with_context(|| format!("Adding {}", file))?;
    }
    Ok(filelist)
}

fn generate_initramfs_overlay<P: glib::IsA<gio::Cancellable>>(
    root: &cap_std::fs::Dir,
    files: &HashSet<String>,
    cancellable: Option<&P>,
) -> Result<fs::File> {
    use std::process::Stdio;
    let filelist = {
        let etcd = root.open_dir("etc")?;
        gather_filelist(&etcd, files, cancellable)?
    };
    let mut out_tmpf = tempfile::tempfile()?;
    // Yeah, we need an initramfs-making crate.  See also
    // https://github.com/dracutdevs/dracut/commit/a9c6704
    let mut cmd = std::process::Command::new("/bin/bash");
    cmd.args([
        "-c",
        "set -euo pipefail; cpio --create --format newc --quiet --reproducible --null | gzip -1",
    ]);
    cmd.cwd_dir(root.try_clone()?);
    let mut child = cmd
        .stdin(Stdio::piped())
        .stdout(Stdio::from(out_tmpf.try_clone()?))
        .spawn()
        .context("spawn")?;
    let mut stdin = std::io::BufWriter::new(child.stdin.take().unwrap());
    // We could make this asynchronous too, but eh
    let inputwriter = std::thread::spawn(move || -> Result<()> {
        for f in filelist {
            stdin.write_all(b"etc/")?;
            stdin.write_all(f.as_bytes())?;
            let nul = [0u8];
            stdin.write_all(&nul)?;
        }
        stdin.flush()?;
        Ok(())
    });
    let status = child.wait()?;
    if !status.success() {
        anyhow::bail!("Failed to generate cpio archive");
    }
    inputwriter.join().unwrap()?;
    out_tmpf.seek(io::SeekFrom::Start(0)).context("seek")?;
    Ok(out_tmpf)
}

fn generate_initramfs_overlay_etc<P: glib::IsA<gio::Cancellable>>(
    files: &HashSet<String>,
    cancellable: Option<&P>,
) -> Result<fs::File> {
    let root = &cap_std::fs::Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
    generate_initramfs_overlay(root, files, cancellable)
}

fn impl_append_dracut_random_cpio(fd: BorrowedFd) -> Result<()> {
    let fd = fd.as_filelike_view::<std::fs::File>();
    let len = (&*fd).seek(std::io::SeekFrom::End(0))?;

    // Add padding; see https://www.kernel.org/doc/Documentation/early-userspace/buffer-format.txt
    //
    // We *always* ensure 4 bytes of NUL padding, no matter what because at
    // least the lz4 decompressor in the kernel uses it as an 'EOF' sign:
    // https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=2c484419efc09e7234c667aa72698cb79ba8d8ed
    let mut padding_len = 4;

    // We also ensure alignment to 4; this isn't strictly required for
    // compressed CPIOs, but matches what e.g. GRUB does. Also xref
    // https://github.com/dracutdevs/dracut/blob/4971f443726360216a4ef3ba8baea258a1cd0f3b/src/dracut-cpio/src// main.rs#L644
    // The final `% ALIGN` handles the case where we're already aligned.
    const ALIGN: u64 = 4;
    padding_len += (ALIGN - (len % ALIGN)) % ALIGN;

    std::io::copy(&mut std::io::repeat(0).take(padding_len), &mut &*fd)?;

    // Generated with: fakeroot /bin/sh -c 'cd dracut-urandom && find . -print0 | sort -z | (mknod dev/random c 1 8 && mknod dev/urandom c 1 9 && cpio -o --null -H newc -R 0:0 --reproducible --quiet -D . -O /tmp/dracut-urandom.cpio)'
    let buf = include_bytes!("../../src/libpriv/dracut-random.cpio.gz");
    (&*fd).write_all(buf)?;
    Ok(())
}

/// Append a small static initramfs chunk which includes /dev/{u,}random, because
/// dracut's FIPS module may fail to do so in restricted environments.
pub(crate) fn append_dracut_random_cpio(fd: i32) -> CxxResult<()> {
    let fd = unsafe { BorrowedFd::borrow_raw(fd) };
    impl_append_dracut_random_cpio(fd).map_err(Into::into)
}

/// cxx-rs entrypoint; we can't use generics and need to return a raw integer for fd
#[context("Generating initramfs overlay")]
pub(crate) fn initramfs_overlay_generate(
    files: &Vec<String>,
    cancellable: Pin<&mut crate::FFIGCancellable>,
) -> CxxResult<i32> {
    let cancellable = &cancellable.gobj_wrap();
    let files: HashSet<String> = files.iter().cloned().collect();
    let r = generate_initramfs_overlay_etc(&files, Some(cancellable))?;
    Ok(r.into_raw_fd())
}

#[context("Running dracut")]
pub(crate) fn run_dracut(root_fs: &Dir, kernel_dir: &str) -> Result<()> {
    let tmp_dir = tempfile::tempdir()?;
    let tmp_initramfs_path = tmp_dir.path().join("initramfs.img");

    let cliwrap_dracut = Utf8Path::new(crate::cliwrap::CLIWRAP_DESTDIR).join("dracut");
    let dracut_path = cliwrap_dracut
        .exists()
        .then_some(cliwrap_dracut)
        .unwrap_or_else(|| Utf8Path::new("dracut").to_owned());
    // If changing this, also look at changing rpmostree-kernel.cxx
    let res = Command::new(dracut_path)
        .args([
            "--no-hostonly",
            "--kver",
            kernel_dir,
            "--reproducible",
            "-v",
            "--add",
            "ostree",
            "-f",
        ])
        .arg(&tmp_initramfs_path)
        .status()?;
    if !res.success() {
        return Err(anyhow!(
            "Failed to generate initramfs (via dracut) for kernel: {kernel_dir}: {:?}",
            res
        ));
    }
    let f = std::fs::OpenOptions::new()
        .append(true)
        .open(&tmp_initramfs_path)?;
    crate::initramfs::append_dracut_random_cpio(f.as_raw_fd())?;
    drop(f);
    let utf8_tmp_dir_path = Utf8Path::from_path(tmp_dir.path().strip_prefix("/")?)
        .context("Error turning Path to Utf8Path")?;
    root_fs.rename(
        utf8_tmp_dir_path.join("initramfs.img"),
        &root_fs,
        (Utf8Path::new("lib/modules").join(kernel_dir)).join("initramfs.img"),
    )?;
    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;
    use cap_std_ext::cap_tempfile;

    #[test]
    fn test_initramfs_overlay() -> Result<()> {
        let cancellable = gio::Cancellable::NONE;
        let tmpd = cap_tempfile::tempdir(cap_std::ambient_authority())?;
        tmpd.create_dir_all("etc/foo")?;
        tmpd.write("etc/foo/somefile", "somecontents")?;
        tmpd.write("etc/foo/otherfile", "othercontents")?;
        let mut h = HashSet::new();
        h.insert("/etc/foo".to_string());
        {
            let mut f = generate_initramfs_overlay(&tmpd, &h, cancellable)?;
            let mut o = tmpd.create("initramfs")?;
            std::io::copy(&mut f, &mut o)?;
        }
        let _ = tmpd.metadata("initramfs").context("stat")?;
        Ok(())
    }

    #[test]
    fn test_append_initramfs() -> Result<()> {
        use std::os::fd::AsFd;
        // Current size of static file
        let dracut_random_len = 171u64;
        let tmpf = tempfile::tempfile()?;
        impl_append_dracut_random_cpio(tmpf.as_fd()).unwrap();
        assert_eq!(tmpf.metadata()?.len(), dracut_random_len + 4);
        let mut tmpf = tempfile::tempfile()?;
        tmpf.write_all(b"x")?;
        impl_append_dracut_random_cpio(tmpf.as_fd()).unwrap();
        assert_eq!(tmpf.metadata()?.len(), dracut_random_len + 8);
        Ok(())
    }
}
