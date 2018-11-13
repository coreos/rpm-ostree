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

use gio_sys;
use glib_sys;
use libc;
use std::ffi::CStr;
use std::ffi::CString;
use std::fmt::Display;
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::{io, ptr};

use openat;

/* Wrapper functions for translating basic types from C to Rust */

/// Convert a C (UTF-8) string to a &str; will panic
/// if it isn't valid UTF-8.  Note the lifetime of
/// the return value must be <= the pointer.
pub fn str_from_nullable<'a>(s: *const libc::c_char) -> Option<&'a str> {
    if s.is_null() {
        None
    } else {
        let s = unsafe { CStr::from_ptr(s) };
        Some(s.to_str().unwrap())
    }
}

/// Given a NUL-terminated C string, convert it to an owned
/// String.  Will panic if the C string is not valid UTF-8.
pub fn string_from_nonnull(s: *const libc::c_char) -> String {
    let buf = bytes_from_nonnull(s);
    String::from_utf8(buf.into()).expect("string_from_nonnull: valid utf-8")
}

/// Convert a C "bytestring" to a OsStr; panics if `s` is `NULL`.
pub fn bytes_from_nonnull<'a>(s: *const libc::c_char) -> &'a [u8] {
    assert!(!s.is_null());
    unsafe { CStr::from_ptr(s) }.to_bytes()
}

pub fn dir_from_dfd(fd: libc::c_int) -> io::Result<openat::Dir> {
    let src = unsafe { openat::Dir::from_raw_fd(fd) };
    let r = src.sub_dir(".")?;
    let _ = src.into_raw_fd();
    Ok(r)
}

/// Assert that a raw pointer is not `NULL`, and cast it to a Rust reference
/// with the static lifetime - it has to be static as we can't tell Rust about
/// our lifetimes from the C side.
pub fn ref_from_raw_ptr<T>(p: *mut T) -> &'static mut T {
    assert!(!p.is_null());
    unsafe { &mut *p }
}

// Functions to map Rust's Error into the "GError convention":
// https://developer.gnome.org/glib/stable/glib-Error-Reporting.html
// Use e.g. int_glib_error() to map to a plain "int" C return.
// return a Result (using the std Error).
// TODO: Try upstreaming this into the glib crate?

pub fn error_to_glib<E: Display>(e: &E, gerror: *mut *mut glib_sys::GError) {
    if gerror.is_null() {
        return;
    }
    unsafe {
        assert!((*gerror).is_null());
        let c_msg = CString::new(e.to_string()).unwrap();
        *gerror = glib_sys::g_error_new_literal(
            gio_sys::g_io_error_quark(),
            gio_sys::G_IO_ERROR_FAILED,
            c_msg.as_ptr(),
        )
    }
}

#[allow(dead_code)]
pub fn int_glib_error<T, E>(res: Result<T, E>, gerror: *mut *mut glib_sys::GError) -> libc::c_int
where
    E: Display,
{
    match res {
        Ok(_) => 1,
        Err(ref e) => {
            error_to_glib(e, gerror);
            0
        }
    }
}

pub fn ptr_glib_error<T, E>(res: Result<Box<T>, E>, gerror: *mut *mut glib_sys::GError) -> *mut T
where
    E: Display,
{
    match res {
        Ok(v) => Box::into_raw(v),
        Err(ref e) => {
            error_to_glib(e, gerror);
            ptr::null_mut()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use glib;
    use std::error::Error;
    use std::{io, ptr};

    struct Foo {
        pub v: u32,
    }
    impl Foo {
        pub fn new() -> Foo {
            Foo { v: 42 }
        }
    }

    #[test]
    fn no_error_ptr() {
        let v = Box::new(Foo::new());
        let r: io::Result<Box<Foo>> = Ok(v);
        let mut error: *mut glib_sys::GError = ptr::null_mut();
        let rawp = ptr_glib_error(r, &mut error);
        assert!(error.is_null());
        assert!(!rawp.is_null());
        // And free it
        let revived_r = unsafe { &*rawp };
        assert!(revived_r.v == 42);
    }

    #[test]
    fn no_error_int() {
        let r: io::Result<()> = Ok(());
        let mut error: *mut glib_sys::GError = ptr::null_mut();
        assert_eq!(int_glib_error(r, &mut error), 1);
        assert!(error.is_null());
    }

    #[test]
    fn throw_error_int() {
        let r: io::Result<()> = Err(io::Error::new(io::ErrorKind::Other, "oops"));
        let mut error: *mut glib_sys::GError = ptr::null_mut();
        assert_eq!(int_glib_error(r, &mut error), 0);
        unsafe {
            assert!(!error.is_null());
            assert_eq!((*error).domain, gio_sys::g_io_error_quark());
            assert_eq!((*error).code, gio_sys::G_IO_ERROR_FAILED);
            let e = glib::Error::wrap(error);
            assert_eq!(e.description(), "oops");
        }
    }

    #[test]
    fn throw_error_ptr() {
        let r: io::Result<Box<Foo>> = Err(io::Error::new(io::ErrorKind::Other, "oops"));
        let mut error: *mut glib_sys::GError = ptr::null_mut();
        assert_eq!(ptr_glib_error(r, &mut error), ptr::null_mut());
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
        assert_eq!(int_glib_error(r, ptr::null_mut()), 0);
    }
}
