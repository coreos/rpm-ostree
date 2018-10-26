/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

extern crate c_utf8;
extern crate curl;
extern crate gio_sys;
extern crate glib;
extern crate glib_sys;
extern crate libc;
extern crate openat;
extern crate tempfile;

#[macro_use]
extern crate serde_derive;
extern crate serde;
extern crate serde_json;
extern crate serde_yaml;

use std::ffi::OsStr;
use std::io::Seek;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::{AsRawFd, IntoRawFd, RawFd};
use std::{fs, io, ptr};

mod ffiutil;
use ffiutil::*;
mod treefile;
use treefile::*;
mod journal;
use journal::*;
mod utils;

// Some of our file descriptors may be read multiple times.
// We try to consistently seek to the start to make that
// convenient from the C side.  Note that this function
// will abort if seek() fails (it really shouldn't).
fn raw_seeked_fd(fd: &mut fs::File) -> RawFd {
    fd.seek(io::SeekFrom::Start(0)).expect("seek");
    fd.as_raw_fd()
}

#[no_mangle]
pub extern "C" fn ror_treefile_new(
    filename: *const libc::c_char,
    arch: *const libc::c_char,
    workdir_dfd: libc::c_int,
    gerror: *mut *mut glib_sys::GError,
) -> *mut Treefile {
    // Convert arguments
    let filename = OsStr::from_bytes(bytes_from_nonnull(filename));
    let arch = str_from_nullable(arch);
    let workdir = match dir_from_dfd(workdir_dfd) {
        Ok(p) => p,
        Err(e) => {
            error_to_glib(&e, gerror);
            return ptr::null_mut();
        }
    };
    // Run code, map error if any, otherwise extract raw pointer, passing
    // ownership back to C.
    ptr_glib_error(
        Treefile::new_boxed(filename.as_ref(), arch, workdir),
        gerror,
    )
}

#[no_mangle]
pub extern "C" fn ror_treefile_get_dfd(tf: *mut Treefile) -> libc::c_int {
    ref_from_raw_ptr(tf).primary_dfd.as_raw_fd()
}

#[no_mangle]
pub extern "C" fn ror_treefile_get_postprocess_script_fd(tf: *mut Treefile) -> libc::c_int {
    ref_from_raw_ptr(tf)
        .externals
        .postprocess_script
        .as_mut()
        .map_or(-1, raw_seeked_fd)
}

#[no_mangle]
pub extern "C" fn ror_treefile_get_add_file_fd(
    tf: *mut Treefile,
    filename: *const libc::c_char,
) -> libc::c_int {
    let tf = ref_from_raw_ptr(tf);
    let filename = OsStr::from_bytes(bytes_from_nonnull(filename));
    let filename = filename.to_string_lossy().into_owned();
    raw_seeked_fd(tf.externals.add_files.get_mut(&filename).expect("add-file"))
}

#[no_mangle]
pub extern "C" fn ror_treefile_get_passwd_fd(tf: *mut Treefile) -> libc::c_int {
    ref_from_raw_ptr(tf)
        .externals
        .passwd
        .as_mut()
        .map_or(-1, raw_seeked_fd)
}

#[no_mangle]
pub extern "C" fn ror_treefile_get_group_fd(tf: *mut Treefile) -> libc::c_int {
    ref_from_raw_ptr(tf)
        .externals
        .group
        .as_mut()
        .map_or(-1, raw_seeked_fd)
}

#[no_mangle]
pub extern "C" fn ror_treefile_get_json_string(tf: *mut Treefile) -> *const libc::c_char {
    ref_from_raw_ptr(tf).serialized.as_ptr()
}

#[no_mangle]
pub extern "C" fn ror_treefile_get_rojig_spec_path(tf: *mut Treefile) -> *const libc::c_char {
    let tf = ref_from_raw_ptr(tf);
    if let &Some(ref rojig) = &tf.rojig_spec {
        rojig.as_ptr()
    } else {
        ptr::null_mut()
    }
}

#[no_mangle]
pub extern "C" fn ror_treefile_free(tf: *mut Treefile) {
    if tf.is_null() {
        return;
    }
    unsafe {
        Box::from_raw(tf);
    }
}

#[no_mangle]
pub extern "C" fn ror_download_to_fd(
    url: *const libc::c_char,
    gerror: *mut *mut glib_sys::GError,
) -> libc::c_int {
    let url = str_from_nullable(url).unwrap();
    match utils::download_url_to_tmpfile(url) {
        Ok(f) => f.into_raw_fd(),
        Err(e) => {
            error_to_glib(&e, gerror);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn ror_journal_print_staging_failure(
    gerror: *mut *mut glib_sys::GError,
) -> libc::c_int {
    int_glib_error(journal_print_staging_failure(), gerror)
}
