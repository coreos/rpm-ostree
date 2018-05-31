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

/* Copied and adapted from:
 * https://github.com/cgwalters/coreos-assembler
 * */

use gio_sys;
use glib_sys;
use libc;
use std;
use std::ffi::CString;
use std::ptr;

// With this, you can just add .map_gerr(error) at the end of functions that
// return a Result.
pub trait ToGErrorConventions {
    fn to_gerr(self: Self, error: *mut *mut glib_sys::GError) -> libc::c_int;
}

impl<T, E> ToGErrorConventions for Result<T, E>
where
    E: std::error::Error,
{
    fn to_gerr(self: Result<T, E>, error: *mut *mut glib_sys::GError) -> libc::c_int {
        match &self {
            &Ok(_) => 1,
            &Err(ref e) => {
                unsafe {
                    assert!(*error == ptr::null_mut());
                    let c_msg = CString::new(e.description()).unwrap();
                    *error = glib_sys::g_error_new_literal(
                        gio_sys::g_io_error_quark(),
                        gio_sys::G_IO_ERROR_FAILED,
                        c_msg.as_ptr(),
                    );
                };
                0
            }
        }
    }
}
