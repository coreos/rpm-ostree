/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//! Helper functions for FFI between C and Rust.  This code
//! is intended to be deprecated and replaced with cxx-rs.
//!
//! This code assumes that it was compiled with the system allocator:
//! https://doc.rust-lang.org/beta/std/alloc/struct.System.html
//! Which means that e.g. returning a Box<str> from Rust can be safely
//! freed on the C side with the C library's `free()`.
//!
//! Panics: As a general rule these functions will panic if provided with invalid
//! input. For example, `ffi_new_string` will panic if provided invalid UTF-8,
//! and `ffi_view_openat_dir` will panic if the file descriptor is invalid.  The
//! rationale here is that if the C state is corrupted, it's possible (likely even)
//! that the Rust side is as well, since (as above) they share a heap allocator.
//!
//! Further, this code all assumes that it was compiled with `panic=abort` mode,
//! since it's undefined behavior to panic across an FFI boundary.  Best practice
//! is to use this FFI code to translate to safe Rust.
//!
//! Naming conventions:
//!
//! Functions named `ffi_view_` do not take ownership of their argument; they
//! should be used to "convert" input parameters from C types to Rust.  Be careful
//! not to store the parameters outside of the function call.
//!
//! Functions named `ffi_new_` create a copy of their inputs, and can safely
//! outlive the function call.

use c_utf8::CUtf8;
use ostree_ext::{gio, glib};
use std::ffi::CString;
use std::fmt::Display;
use std::os::unix::io::{FromRawFd, IntoRawFd};
use std::ptr;

// View `fd` as an `openat::Dir` instance.  Lifetime of return value
// must be less than or equal to that of parameter.
pub(crate) fn ffi_view_openat_dir(fd: libc::c_int) -> openat::Dir {
    let src = unsafe { openat::Dir::from_raw_fd(fd) };
    let r = src.sub_dir(".").expect("ffi_view_openat_dir");
    let _ = src.into_raw_fd();
    r
}

// View `fd` as an `Option<openat::Dir>` instance.  Lifetime of return value
// must be less than or equal to that of parameter.
#[allow(dead_code)]
pub(crate) fn ffi_view_openat_dir_option(fd: libc::c_int) -> Option<openat::Dir> {
    match fd {
        -1 => None,
        _ => Some(ffi_view_openat_dir(fd)),
    }
}

// Functions to map Rust's Error into the "GError convention":
// https://developer.gnome.org/glib/stable/glib-Error-Reporting.html
// Use e.g. int_glib_error() to map to a plain "int" C return.
// return a Result (using the std Error).
// TODO: Try upstreaming this into the glib crate?

/// Convert C results (int + GError convention) to anyhow.
#[allow(dead_code)]
pub(crate) fn int_gerror_to_result(res: i32, gerror: *mut glib::ffi::GError) -> anyhow::Result<()> {
    if res != 0 {
        Ok(())
    } else {
        assert!(!gerror.is_null(), "invalid failure, NULL Gerror");
        let err_msg = unsafe { CUtf8::from_ptr((*gerror).message) }?;
        anyhow::bail!("{}", err_msg)
    }
}

pub(crate) fn error_to_glib<E: Display>(e: &E, gerror: *mut *mut glib::ffi::GError) {
    if gerror.is_null() {
        return;
    }
    unsafe {
        assert!((*gerror).is_null());
        let c_msg = CString::new(format!("{:#}", e)).unwrap();
        *gerror = glib::ffi::g_error_new_literal(
            gio::ffi::g_io_error_quark(),
            gio::ffi::G_IO_ERROR_FAILED,
            c_msg.as_ptr(),
        )
    }
}

#[allow(dead_code)]
pub(crate) fn int_glib_error<T, E>(
    res: Result<T, E>,
    gerror: *mut *mut glib::ffi::GError,
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

#[allow(dead_code)]
pub(crate) fn ptr_glib_error<T, E>(
    res: Result<Box<T>, E>,
    gerror: *mut *mut glib::ffi::GError,
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
        let mut error: *mut glib::ffi::GError = ptr::null_mut();
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
        let mut error: *mut glib::ffi::GError = ptr::null_mut();
        assert_eq!(int_glib_error(r, &mut error), 1);
        assert!(error.is_null());
    }

    #[test]
    fn throw_error_int() {
        let r: io::Result<()> = Err(io::Error::new(io::ErrorKind::Other, "oops"));
        let mut error: *mut glib::ffi::GError = ptr::null_mut();
        assert_eq!(int_glib_error(r, &mut error), 0);
        unsafe {
            assert!(!error.is_null());
            assert_eq!((*error).domain, gio::ffi::g_io_error_quark());
            assert_eq!((*error).code, gio::ffi::G_IO_ERROR_FAILED);
            let e: glib::Error = glib::translate::from_glib_full(error);
            assert_eq!(e.to_string(), "oops");
        }
    }

    #[test]
    fn throw_error_ptr() {
        let r: io::Result<Box<Foo>> = Err(io::Error::new(io::ErrorKind::Other, "oops"));
        let mut error: *mut glib::ffi::GError = ptr::null_mut();
        assert_eq!(ptr_glib_error(r, &mut error), ptr::null_mut());
        unsafe {
            assert!(!error.is_null());
            assert_eq!((*error).domain, gio::ffi::g_io_error_quark());
            assert_eq!((*error).code, gio::ffi::G_IO_ERROR_FAILED);
            let e: glib::Error = glib::translate::from_glib_full(error);
            assert_eq!(e.to_string(), "oops");
        }
    }

    #[test]
    fn throw_error_ignored() {
        let r: io::Result<()> = Err(io::Error::new(io::ErrorKind::Other, "oops"));
        assert_eq!(int_glib_error(r, ptr::null_mut()), 0);
    }
}
