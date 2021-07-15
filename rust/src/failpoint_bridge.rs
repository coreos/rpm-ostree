//! C++ bindings for the failpoint crate.
// SPDX-License-Identifier: Apache-2.0 OR MIT

/// Expose the `fail::fail_point` macro to C++.
pub fn failpoint(p: &str) {
    glib::g_debug!("rpm-ostree", "{}", p);
    fail::fail_point!(p);
}
