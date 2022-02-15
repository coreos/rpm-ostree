//! Generate an "overlay" initramfs image
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use anyhow::{Context, Result};
use camino::Utf8Path;
use cap_std_ext::cap_std;
use cap_std_ext::prelude::CapStdExtCommandExt;
use ostree_ext::{gio, glib, prelude::*};
use std::collections::BTreeSet;
use std::collections::HashSet;
use std::convert::TryInto;
use std::io::prelude::*;
use std::os::unix::io::IntoRawFd;
use std::path::{Component, Path, PathBuf};
use std::pin::Pin;
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
        let _ = c.set_error_if_cancelled()?;
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
    cmd.args(&[
        "-c",
        "set -euo pipefail; cpio --create --format newc --quiet --reproducible --null | gzip -1",
    ]);
    cmd.cwd_dir_owned(root.try_clone()?);
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

pub(crate) fn get_dracut_random_cpio() -> &'static [u8] {
    // Generated with: fakeroot /bin/sh -c 'cd dracut-urandom && find . -print0 | sort -z | (mknod dev/random c 1 8 && mknod dev/urandom c 1 9 && cpio -o --null -H newc -R 0:0 --reproducible --quiet -D . -O /tmp/dracut-urandom.cpio)'
    include_bytes!("../../src/libpriv/dracut-random.cpio.gz")
}

/// cxx-rs entrypoint; we can't use generics and need to return a raw integer for fd
pub(crate) fn initramfs_overlay_generate(
    files: &Vec<String>,
    mut cancellable: Pin<&mut crate::FFIGCancellable>,
) -> CxxResult<i32> {
    let cancellable = &cancellable.gobj_wrap();
    let files: HashSet<String> = files.iter().cloned().collect();
    let r = generate_initramfs_overlay_etc(&files, Some(cancellable))?;
    Ok(r.into_raw_fd())
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_initramfs_overlay() -> Result<()> {
        let cancellable = gio::NONE_CANCELLABLE;
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
}
