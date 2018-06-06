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

// Consume a Result into the "GError convention":
// https://developer.gnome.org/glib/stable/glib-Error-Reporting.html
// To use, just add .to_glib_convention(error) at the end of function calls that
// return a Result (using the std Error).
pub trait ToGlibConvention {
    fn to_glib_convention(self: Self, error: *mut *mut glib_sys::GError) -> libc::c_int;
}

// TODO: Add a variant for io::Result?  Or try upstreaming this into the glib crate?
impl<T, E> ToGlibConvention for Result<T, E>
where
    E: std::error::Error,
{
    fn to_glib_convention(self: Result<T, E>, error: *mut *mut glib_sys::GError) -> libc::c_int {
        match &self {
            &Ok(_) => 1,
            &Err(ref e) => {
                if !error.is_null() {
                    unsafe {
                        assert!((*error).is_null());
                        let c_msg = CString::new(e.description()).unwrap();
                        *error = glib_sys::g_error_new_literal(
                            gio_sys::g_io_error_quark(),
                            gio_sys::G_IO_ERROR_FAILED,
                            c_msg.as_ptr(),
                        )
                    }
                };
                0
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use glib;
    use std::error::Error;
    use std::{io, ptr};

    #[test]
    fn no_error() {
        let r: io::Result<()> = Ok(());
        let mut error: *mut glib_sys::GError = ptr::null_mut();
        assert_eq!(r.to_glib_convention(&mut error), 1);
        assert!(error.is_null());
    }

    #[test]
    fn throw_error() {
        let r: io::Result<()> = Err(io::Error::new(io::ErrorKind::Other, "oops"));
        let mut error: *mut glib_sys::GError = ptr::null_mut();
        assert_eq!(r.to_glib_convention(&mut error), 0);
        unsafe {
            assert!(!error.is_null());
            assert_eq!((*error).domain, gio_sys::g_io_error_quark());
            assert_eq!((*error).code, gio_sys::G_IO_ERROR_FAILED);
            let e = glib::Error::wrap(error);
            assert_eq!(e.description(), "oops");
        }
    }

    #[test]
    fn throw_error_ignored() {
        let r: io::Result<()> = Err(io::Error::new(io::ErrorKind::Other, "oops"));
        assert_eq!(r.to_glib_convention(ptr::null_mut()), 0);
    }
}
