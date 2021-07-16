//! Generate an "overlay" initramfs image
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::*;
use anyhow::{Context, Result};
use camino::Utf8Path;
use gio::prelude::*;
use openat::SimpleType;
use std::collections::BTreeSet;
use std::collections::HashSet;
use std::convert::TryInto;
use std::fs;
use std::io;
use std::io::prelude::*;
use std::os::unix::io::AsRawFd;
use std::os::unix::io::IntoRawFd;
use std::path::{Component, Path, PathBuf};
use std::pin::Pin;
use std::rc;
use subprocess::Exec;

fn list_files_recurse<P: glib::IsA<gio::Cancellable>>(
    d: &openat::Dir,
    path: &str,
    filelist: &mut BTreeSet<String>,
    cancellable: Option<&P>,
) -> Result<()> {
    // Maybe add a glib feature for openat-ext to support cancellability?  Or
    // perhaps we should just use futures and ensure GCancellable bridges to that.
    if let Some(c) = cancellable {
        let _ = c.set_error_if_cancelled()?;
    }
    let meta = d.metadata(path).context("stat")?;
    match meta.simple_type() {
        SimpleType::Symlink | SimpleType::File => {}
        SimpleType::Dir => {
            for e in d.list_dir(path).context("readdir")? {
                let e = e.context("readdir")?;
                let name: &Utf8Path = Path::new(e.file_name()).try_into()?;
                let subpath = format!("{}/{}", path, name);
                list_files_recurse(&d, &subpath, filelist, cancellable)?;
            }
        }
        _ => anyhow::bail!("Invalid non-regfile/symlink/directory: {}", path),
    }
    if !filelist.contains(path) {
        filelist.insert(path.to_string());
    }
    Ok(())
}

fn gather_filelist<P: glib::IsA<gio::Cancellable>>(
    d: &openat::Dir,
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

        list_files_recurse(&d, file, &mut filelist, cancellable)
            .with_context(|| format!("Adding {}", file))?;
    }
    Ok(filelist)
}

fn generate_initramfs_overlay<P: glib::IsA<gio::Cancellable>>(
    root: &openat::Dir,
    files: &HashSet<String>,
    cancellable: Option<&P>,
) -> Result<fs::File> {
    let filelist = {
        let etcd = root.sub_dir("etc")?;
        gather_filelist(&etcd, files, cancellable)?
    };
    let out_tmpf = std::rc::Rc::new(tempfile::tempfile()?);
    {
        let cmd = Exec::cmd("cpio")
            .args(&[
                "--create",
                "--format",
                "newc",
                "--quiet",
                "--reproducible",
                "--null",
            ])
            .cwd(format!("/proc/self/fd/{}", root.as_raw_fd()))
            | Exec::cmd("gzip").arg("-1");
        let mut children = cmd
            .stdin(subprocess::Redirection::Pipe)
            .stdout(subprocess::Redirection::RcFile(out_tmpf.clone()))
            .popen()
            .context("spawn")?;
        {
            let mut cstdin = std::io::BufWriter::new(children[0].stdin.take().expect("stdin"));
            for f in filelist {
                cstdin.write_all(b"etc/")?;
                cstdin.write_all(f.as_bytes())?;
                let nul = [0u8];
                cstdin.write_all(&nul)?;
            }
            cstdin.flush()?;
        }
        for mut child in children {
            let status = child.wait()?;
            if !status.success() {
                anyhow::bail!("Failed to generate cpio archive");
            }
        }
    }
    let mut out_tmpf = rc::Rc::try_unwrap(out_tmpf).expect("initramfs rc unexpected count");
    out_tmpf.seek(io::SeekFrom::Start(0)).context("seek")?;
    Ok(out_tmpf)
}

fn generate_initramfs_overlay_etc<P: glib::IsA<gio::Cancellable>>(
    files: &HashSet<String>,
    cancellable: Option<&P>,
) -> Result<fs::File> {
    let root = openat::Dir::open("/")?;
    generate_initramfs_overlay(&root, files, cancellable)
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
    use openat_ext::FileExt;

    #[test]
    fn test_initramfs_overlay() -> Result<()> {
        let cancellable = gio::NONE_CANCELLABLE;
        let tmpd = tempfile::tempdir()?;
        std::fs::create_dir_all(tmpd.path().join("etc/foo"))?;
        std::fs::write(tmpd.path().join("etc/foo/somefile"), "somecontents")?;
        std::fs::write(tmpd.path().join("etc/foo/otherfile"), "othercontents")?;
        let tmpd = openat::Dir::open(tmpd.path())?;
        let mut h = HashSet::new();
        h.insert("/etc/foo".to_string());
        {
            let f = generate_initramfs_overlay(&tmpd, &h, cancellable)?;
            let o = tmpd.new_file("initramfs", 0o644)?;
            f.copy_to(&o)?;
        }
        let _ = tmpd.metadata("initramfs").context("stat")?;
        Ok(())
    }
}
