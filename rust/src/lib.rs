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

use std::ffi::{CStr, OsStr};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::FromRawFd;
use std::path::Path;
use std::{fs, io};

mod glibutils;
use glibutils::*;
mod treefile;
use treefile::treefile_read_impl;

/* Wrapper functions for translating from C to Rust */

#[no_mangle]
pub extern "C" fn rpmostree_rs_treefile_read(
    filename: *const libc::c_char,
    fd: libc::c_int,
    error: *mut *mut glib_sys::GError,
) -> libc::c_int {
    // using an O_TMPFILE is an easy way to avoid ownership transfer issues w/ returning allocated
    // memory across the Rust/C boundary

    // let's just get the unsafory stuff (see what I did there?) out of the way first
    let output = io::BufWriter::new(unsafe { fs::File::from_raw_fd(libc::dup(fd)) });
    let c_str: &CStr = unsafe { CStr::from_ptr(filename) };
    let filename_path = Path::new(OsStr::from_bytes(c_str.to_bytes()));

    treefile_read_impl(filename_path, output).to_glib_convention(error)
}
