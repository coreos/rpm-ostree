//! Helper APIs for interacting with ostree-based operating systems.

// SPDX-License-Identifier: Apache-2.0 OR MIT

#![deny(unused_must_use)]

mod binding_fixes;

pub mod prelude {
    pub use super::binding_fixes::*;
}
