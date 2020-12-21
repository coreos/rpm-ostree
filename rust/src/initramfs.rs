//! Generate an "overlay" initramfs image

use anyhow::{Context, Result};
use gio::prelude::*;
use openat::SimpleType;
use std::collections::BTreeSet;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io;
use std::io::prelude::*;
use std::os::unix::io::AsRawFd;
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
                let name = e.file_name();
                let name = match name.to_str() {
                    Some(n) => n,
                    None => anyhow::bail!("Invalid UTF-8 name {}", name.to_string_lossy()),
                };
                let subpath = format!("{}/{}", path, name);
                list_files_recurse(&d, &subpath, filelist, cancellable)?;
            }
        }
        _ => anyhow::bail!("Invalid non-regfile/symlink/directory: {}", path),
    }
    filelist.insert(path.to_string());
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

mod ffi {
    use super::*;
    use crate::ffiutil::error_to_glib;
    use glib::translate::*;
    use std::os::unix::io::IntoRawFd;

    #[no_mangle]
    pub extern "C" fn ror_initramfs_overlay_generate(
        files: *mut glib_sys::GHashTable,
        out_fd: *mut libc::c_int,
        cancellable: *mut gio_sys::GCancellable,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        // TODO glib-rs doesn't allow directly converting to HashSet;
        // probably best to fix the calling code to pass a GStrv anyways.
        let mut files: HashMap<String, String> =
            unsafe { FromGlibPtrContainer::from_glib_none(files) };
        let files: HashSet<String> = files.drain().map(|(k, _)| k).collect();
        let cancellable: Option<gio::Cancellable> = unsafe { from_glib_none(cancellable) };
        let cancellable = cancellable.as_ref();
        match generate_initramfs_overlay_etc(&files, cancellable) {
            Ok(fd) => {
                unsafe {
                    *out_fd = fd.into_raw_fd();
                }
                1
            }
            Err(ref e) => {
                error_to_glib(e, gerror);
                0
            }
        }
    }
}
pub use self::ffi::*;
