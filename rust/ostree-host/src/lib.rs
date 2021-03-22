//! Helper APIs for interacting with ostree-based operating systems.

// SPDX-License-Identifier: Apache-2.0 OR MIT

#![deny(unused_must_use)]

mod binding_fixes;
pub use binding_fixes::*;
mod sysroot_ext;
pub use sysroot_ext::*;

/// This prelude currently just adds extension traits to various OSTree objects,
/// and like all preludes is intended to be quite "safe" to import and will avoid
/// likely naming clashes.
pub mod prelude {
    pub use super::binding_fixes::*;
    pub use super::sysroot_ext::*;
}
