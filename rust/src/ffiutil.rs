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

use cap_std_ext::{cap_std, rustix};

/// Create a new cap_std directory for an openat version.
/// This creates a new file descriptor, because we can't guarantee they are
/// interchangable; for example right now openat uses `O_PATH`
pub(crate) unsafe fn ffi_dirfd(fd: libc::c_int) -> std::io::Result<cap_std::fs::Dir> {
    let fd = unsafe { rustix::fd::BorrowedFd::borrow_raw(fd) };
    cap_std::fs::Dir::reopen_dir(&fd)
}
