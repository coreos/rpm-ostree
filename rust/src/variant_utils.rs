//! Helpers for GVariant until we can use a newer glib crate with https://github.com/gtk-rs/glib/pull/651
// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::{borrow::Cow, mem::size_of};

use glib::translate::*;

// These constants should really be in gtk-rs
lazy_static::lazy_static! {
    pub(crate) static ref TY_S: &'static glib::VariantTy = {
        glib::VariantTy::new("s").unwrap()
    };
    pub(crate) static ref TY_B: &'static glib::VariantTy = {
        glib::VariantTy::new("b").unwrap()
    };
    pub(crate) static ref TY_U: &'static glib::VariantTy = {
        glib::VariantTy::new("u").unwrap()
    };
    pub(crate) static ref TY_Y: &'static glib::VariantTy = {
        glib::VariantTy::new("y").unwrap()
    };
}

pub(crate) fn new_variant_tuple<'a>(
    items: impl IntoIterator<Item = &'a glib::Variant>,
) -> glib::Variant {
    let v: Vec<_> = items.into_iter().map(|v| v.to_glib_none().0).collect();
    unsafe {
        let r = glib_sys::g_variant_new_tuple(v.as_ptr(), v.len());
        glib_sys::g_variant_ref_sink(r);
        from_glib_full(r)
    }
}

pub(crate) fn new_variant_bytearray(buf: &[u8]) -> glib::Variant {
    unsafe {
        let r = glib_sys::g_variant_new_fixed_array(
            b"y\0".as_ptr() as *const _,
            buf.as_ptr() as *const _,
            buf.len(),
            size_of::<u8>(),
        );
        glib_sys::g_variant_ref_sink(r);
        from_glib_full(r)
    }
}

pub(crate) fn new_variant_a_ayay<T: AsRef<[u8]>>(items: &[(T, T)]) -> glib::Variant {
    unsafe {
        let ty = glib::VariantTy::new("a(ayay)").unwrap();
        let builder = glib_sys::g_variant_builder_new(ty.as_ptr() as *const _);
        for (k, v) in items {
            let k = new_variant_bytearray(k.as_ref());
            let v = new_variant_bytearray(v.as_ref());
            let val = new_variant_tuple(&[k, v]);
            glib_sys::g_variant_builder_add_value(builder, val.to_glib_none().0);
        }
        let v = glib_sys::g_variant_builder_end(builder);
        glib_sys::g_variant_ref_sink(v);
        from_glib_full(v)
    }
}

pub(crate) fn variant_tuple_get(v: &glib::Variant, n: usize) -> Option<glib::Variant> {
    let v = v.to_glib_none();
    let l = unsafe { glib_sys::g_variant_n_children(v.0) };
    if n >= l {
        None
    } else {
        unsafe { from_glib_full(glib_sys::g_variant_get_child_value(v.0, n)) }
    }
}

pub(crate) fn new_variant_array(ty: &glib::VariantTy, children: &[glib::Variant]) -> glib::Variant {
    unsafe {
        let r = glib_sys::g_variant_new_array(
            ty.as_ptr() as *const _,
            children.to_glib_none().0,
            children.len(),
        );
        glib_sys::g_variant_ref_sink(r);
        from_glib_full(r)
    }
}

pub(crate) fn is_container(v: &glib::Variant) -> bool {
    unsafe { glib_sys::g_variant_is_container(v.to_glib_none().0) != glib_sys::GFALSE }
}

pub(crate) fn n_children(v: &glib::Variant) -> usize {
    assert!(is_container(v));
    unsafe { glib_sys::g_variant_n_children(v.to_glib_none().0) }
}

/// Find a string value in a GVariantDict
pub(crate) fn variant_dict_lookup_str(v: &glib::VariantDict, k: &str) -> Option<String> {
    // Unwrap safety: Passing the GVariant type string gives us the right value type
    v.lookup_value(k, Some(*TY_S))
        .map(|v| v.get_str().unwrap().to_string())
}

/// Find a boolean value in a GVariantDict
pub(crate) fn variant_dict_lookup_bool(v: &glib::VariantDict, k: &str) -> Option<bool> {
    // Unwrap safety: Passing the GVariant type string gives us the right value type
    v.lookup_value(k, Some(*TY_B)).map(|v| v.get().unwrap())
}

pub(crate) fn byteswap_be_to_native(v: &glib::Variant) -> Cow<glib::Variant> {
    if cfg!(target_endian = "big") {
        Cow::Borrowed(v)
    } else {
        unsafe {
            let r = glib_sys::g_variant_byteswap(v.to_glib_none().0);
            Cow::Owned(from_glib_full(r))
        }
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use anyhow::Result;
    use glib::ToVariant;

    #[test]
    fn tuple() -> Result<()> {
        let t = &new_variant_tuple(&["hello".to_variant(), "world".to_variant()]);
        assert_eq!(variant_tuple_get(t, 0).unwrap().get_str().unwrap(), "hello");
        assert_eq!(variant_tuple_get(t, 1).unwrap().get_str().unwrap(), "world");
        assert!(variant_tuple_get(t, 2).is_none());
        assert!(variant_tuple_get(t, 42).is_none());
        Ok(())
    }
}
