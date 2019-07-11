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
use openat;
use std::ffi::CString;
use std::ffi::{CStr, OsStr};
use std::fmt::Display;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::ptr;

/* Helper functions for FFI between C and Rust.
 *
 * This code assumes that it was compiled with the system allocator:
 * https://doc.rust-lang.org/beta/std/alloc/struct.System.html
 * Which means that e.g. returning a Box<str> from Rust can be safely
 * freed on the C side with the C library's `free()`.
 *
 * Panics: As a general rule these functions will panic if provided with invalid
 * input. For example, `ffi_new_string` will panic if provided invalid UTF-8,
 * and `ffi_view_openat_dir` will panic if the file descriptor is invalid.  The
 * rationale here is that if the C state is corrupted, it's possible (likely even)
 * that the Rust side is as well, since (as above) they share a heap allocator.
 *
 * Further, this code all assumes that it was compiled with `panic=abort` mode,
 * since it's undefined behavior to panic across an FFI boundary.  Best practice
 * is to use this FFI code to translate to safe Rust.
 *
 * Naming conventions:
 *
 * Functions named `ffi_view_` do not take ownership of their argument; they
 * should be used to "convert" input parameters from C types to Rust.  Be careful
 * not to store the parameters outside of the function call.
 *
 * Functions named `ffi_new_` create a copy of their inputs, and can safely
 * outlive the function call.
 */

/// Convert a C (UTF-8) string to a &str; will panic
/// if it is NULL or isn't valid UTF-8.  Note the lifetime of
/// the return value must be <= the pointer.
pub(crate) fn ffi_view_str<'a>(s: *const libc::c_char) -> &'a str {
    assert!(!s.is_null());
    let s = unsafe { CStr::from_ptr(s) };
    s.to_str().expect("ffi_view_str: valid utf-8")
}

/// Convert a C (UTF-8) string to a &str; will panic
/// if it isn't valid UTF-8.  Note the lifetime of
/// the return value must be <= the pointer.
pub(crate) fn ffi_view_nullable_str<'a>(s: *const libc::c_char) -> Option<&'a str> {
    if s.is_null() {
        None
    } else {
        Some(ffi_view_str(s))
    }
}

/// Given a NUL-terminated C string, copy it to an owned
/// String.  Will panic if the C string is not valid UTF-8.
pub(crate) fn ffi_new_string(s: *const libc::c_char) -> String {
    let buf = ffi_view_bytestring(s);
    String::from_utf8(buf.into()).expect("ffi_new_string: valid utf-8")
}

/// View a C "bytestring" (NUL terminated) as a Rust byte array.
/// Panics if `s` is `NULL`.
pub(crate) fn ffi_view_bytestring<'a>(s: *const libc::c_char) -> &'a [u8] {
    assert!(!s.is_null());
    unsafe { CStr::from_ptr(s) }.to_bytes()
}

/// View a C "bytestring" (NUL terminated) as a Rust OsStr.
/// Panics if `s` is `NULL`.
pub(crate) fn ffi_view_os_str<'a>(s: *const libc::c_char) -> &'a OsStr {
    OsStr::from_bytes(ffi_view_bytestring(s))
}

/// Transform a GPtrArray to a Vec. There's no converter in glib yet to do this. See related
/// discussions in: https://github.com/gtk-rs/glib/pull/482
pub(crate) fn ffi_ptr_array_to_vec<T>(a: *mut glib_sys::GPtrArray) -> Vec<*mut T> {
    assert!(!a.is_null());

    let n = unsafe { (*a).len } as usize;
    let mut v = Vec::with_capacity(n);
    unsafe {
        for i in 0..n {
            v.push(ptr::read((*a).pdata.add(i as usize)) as *mut T);
        }
    }
    v
}

/// Transform a NULL-terminated array of C strings to an allocated Vec holding unowned references.
/// This is similar to FromGlibPtrContainer::from_glib_none(), but avoids cloning elements.
pub(crate) fn ffi_strv_to_os_str_vec<'a>(mut strv: *mut *mut libc::c_char) -> Vec<&'a OsStr> {
    assert!(!strv.is_null());

    // In theory, we could use std::slice::from_raw_parts instead to make this more 0-cost and
    // create an &[*mut libc::c_char], but from there there's no clean way to get a &['a OsStr]
    // short of just transmuting.
    let mut v = Vec::new();
    unsafe {
        while !(*strv).is_null() {
            v.push(ffi_view_os_str(*strv));
            strv = strv.offset(1);
        }
    }
    v
}

// View `fd` as an `openat::Dir` instance.  Lifetime of return value
// must be less than or equal to that of parameter.
pub(crate) fn ffi_view_openat_dir(fd: libc::c_int) -> openat::Dir {
    let src = unsafe { openat::Dir::from_raw_fd(fd) };
    let r = src.sub_dir(".").expect("ffi_view_openat_dir");
    let _ = src.into_raw_fd();
    r
}

/// Assert that a raw pointer is not `NULL`, and cast it to a Rust reference
/// with the static lifetime - it has to be static as we can't tell Rust about
/// our lifetimes from the C side.
pub(crate) fn ref_from_raw_ptr<T>(p: *mut T) -> &'static mut T {
    assert!(!p.is_null());
    unsafe { &mut *p }
}

// Functions to map Rust's Error into the "GError convention":
// https://developer.gnome.org/glib/stable/glib-Error-Reporting.html
// Use e.g. int_glib_error() to map to a plain "int" C return.
// return a Result (using the std Error).
// TODO: Try upstreaming this into the glib crate?

pub(crate) fn error_to_glib<E: Display>(e: &E, gerror: *mut *mut glib_sys::GError) {
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
pub(crate) fn int_glib_error<T, E>(
    res: Result<T, E>,
    gerror: *mut *mut glib_sys::GError,
) -> libc::c_int
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

pub(crate) fn ptr_glib_error<T, E>(
    res: Result<Box<T>, E>,
    gerror: *mut *mut glib_sys::GError,
) -> *mut T
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
