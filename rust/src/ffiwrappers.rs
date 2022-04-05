/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//! Wrappers for stdcall imports.

use anyhow::Result;
use std::ffi::CString;

use ostree_ext::glib::ffi::g_variant_is_object_path;

// FIXME: Remove this once https://github.com/gtk-rs/gtk-rs-core/issues/622
// is fixed.
pub(crate) fn is_object_path(str: &str) -> Result<bool> {
    let str = CString::new(str)?;
    unsafe { Ok(g_variant_is_object_path(str.as_ptr()) != 0) }
}
