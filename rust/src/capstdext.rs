//! Helper functions for the [`cap-std` crate].
//!
//! [`cap-std` crate]: https://crates.io/crates/cap-std
//  SPDX-License-Identifier: Apache-2.0 OR MIT

use cap_std::fs::DirBuilder;
use cap_std_ext::cap_std;
use std::os::unix::fs::DirBuilderExt;

pub(crate) fn dirbuilder_from_mode(m: u32) -> DirBuilder {
    let mut r = DirBuilder::new();
    r.mode(m);
    r
}
