//! Helpers for GVariant.  Ideally, any code here is also submitted as a PR to glib-rs.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::borrow::Cow;

use glib::translate::*;
use ostree_ext::glib;

pub(crate) fn byteswap_be_to_native(v: &glib::Variant) -> Cow<'_, glib::Variant> {
    if cfg!(target_endian = "big") {
        Cow::Borrowed(v)
    } else {
        unsafe {
            let r = glib::ffi::g_variant_byteswap(v.to_glib_none().0);
            Cow::Owned(from_glib_full(r))
        }
    }
}
