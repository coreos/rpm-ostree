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

#[macro_use]
extern crate serde_derive;
extern crate serde;
extern crate serde_json;
extern crate serde_yaml;

use std::ffi::CStr;
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::path::Path;
use std::{fs, io};

mod glibutils;
use glibutils::*;
mod treefile;
use treefile::treefile_read_impl;

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

#[no_mangle]
pub extern "C" fn rpmostree_rs_treefile_read(
    filename: *const libc::c_char,
    arch: *const libc::c_char,
    fd: libc::c_int,
    error: *mut *mut glib_sys::GError,
) -> libc::c_int {
    // Convert arguments
    let filename = Path::new(unsafe { CStr::from_ptr(filename).to_str().unwrap() });
    let arch = str_from_nullable(arch);
    // using an O_TMPFILE is an easy way to avoid ownership transfer issues w/ returning allocated
    // memory across the Rust/C boundary
    // The dance with `file` is to avoid dup()ing the fd unnecessarily
    let file = unsafe { fs::File::from_raw_fd(fd) };
    let r = {
        let output = io::BufWriter::new(&file);
        treefile_read_impl(filename, arch, output).to_glib_convention(error)
    };
    file.into_raw_fd(); // Drop ownership of the FD again
    r
}
