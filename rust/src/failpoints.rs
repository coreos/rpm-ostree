//! Wrappers and utilities on top of the `fail` crate.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;

/// TODO: Use https://github.com/tikv/fail-rs/pull/68 once it merges
#[macro_export]
macro_rules! try_fail_point {
    ($name:expr) => {{
        if let Some(e) = fail::eval($name, |msg| {
            let msg = msg.unwrap_or_else(|| "synthetic failpoint".to_string());
            anyhow::Error::msg(msg)
        }) {
            return Err(From::from(e));
        }
    }};
    ($name:expr, $cond:expr) => {{
        if $cond {
            $crate::try_fail_point!($name);
        }
    }};
}

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
