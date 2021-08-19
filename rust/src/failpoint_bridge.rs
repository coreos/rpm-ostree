//! C++ bindings for the failpoint crate.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;

/// Expose the `fail::fail_point` macro to C++.
pub fn failpoint(p: &str) -> Result<()> {
    ostree_ext::glib::g_debug!("rpm-ostree", "{}", p);
    fail::fail_point!(p, |r| {
        Err(match r {
            Some(ref msg) => anyhow::anyhow!("{}", msg),
            None => anyhow::anyhow!("failpoint {}", p),
        })
    });
    Ok(())
}
