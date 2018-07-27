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

extern crate gio_sys;
extern crate glib;
extern crate glib_sys;
extern crate libc;
extern crate tempfile;

#[macro_use]
extern crate serde_derive;
extern crate serde;
extern crate serde_json;
extern crate serde_yaml;

use std::ffi::{CStr, OsStr};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::AsRawFd;

mod glibutils;
use glibutils::*;
mod treefile;
use treefile::{treefile_read_impl, Treefile};

/* Wrapper functions for translating from C to Rust */

/// Convert a C (UTF-8) string to a &str; will panic
/// if it isn't valid UTF-8.  Note the lifetime of
/// the return value must be <= the pointer.
fn str_from_nullable<'a>(s: *const libc::c_char) -> Option<&'a str> {
    if s.is_null() {
        None
    } else {
        let s = unsafe { CStr::from_ptr(s) };
        Some(s.to_str().unwrap())
    }
}

/// Convert a C "bytestring" to a OsStr; panics if `s` is `NULL`.
fn bytes_from_nonnull<'a>(s: *const libc::c_char) -> &'a [u8] {
    assert!(!s.is_null());
    unsafe { CStr::from_ptr(s) }.to_bytes()
}

#[no_mangle]
pub extern "C" fn rpmostree_rs_treefile_new(
    filename: *const libc::c_char,
    arch: *const libc::c_char,
    error: *mut *mut glib_sys::GError,
) -> *mut Treefile {
    // Convert arguments
    let filename = OsStr::from_bytes(bytes_from_nonnull(filename));
    let arch = str_from_nullable(arch);
    // Run code, map error if any, otherwise extract raw pointer, passing
    // ownership back to C.
    ptr_glib_error(treefile_read_impl(filename.as_ref(), arch), error)
}

#[no_mangle]
pub extern "C" fn rpmostree_rs_treefile_get_json_fd(tf: *mut Treefile) -> libc::c_int {
    assert!(!tf.is_null());
    let tf = unsafe { &mut *tf };
    tf.json_fd.as_raw_fd() as libc::c_int
}

#[no_mangle]
pub extern "C" fn rpmostree_rs_treefile_free(tf: *mut Treefile) {
    if tf.is_null() {
        return;
    }
    unsafe {
        Box::from_raw(tf);
    }
}
