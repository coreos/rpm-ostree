//! Wrappers that bridge the world of cxx.rs and GObject-introspect based crate bindings.
//! See https://github.com/dtolnay/cxx/issues/544
//! While cxx.rs supports including externally-bound types (like ostree::Repo),
//! two things make this more complicated.  First, cxx.rs requires implementing
//! a trait, and due to the orphan rule we can't implement foreign traits on
//! foreign types.  Second, what we *actually* want on the Rust side isn't
//! the *_sys type (e.g. ostree_sys::OstreeRepo) but the `ostree::Repo` type.
//! So for now, we define a `FFIGObjectWrapper` trait that helps with this.
//! In the future though hopefully cxx.rs improves this situation.

use cxx::{type_id, ExternType};
use paste::paste;

/// A custom trait used to translate a *_sys C type wrapper
/// to its GObject version.
pub trait FFIGObjectWrapper {
    type Wrapper;

    fn gobj_wrap(&mut self) -> Self::Wrapper;
}

/// Implement FFIGObjectWrapper given a pair of wrapper type
/// and sys type.
macro_rules! impl_wrap {
    ($w:ident, $bound:path) => {
        impl FFIGObjectWrapper for $w {
            type Wrapper = $bound;
            fn gobj_wrap(&mut self) -> Self::Wrapper {
                unsafe { glib::translate::from_glib_none(&mut self.0 as *mut _) }
            }
        }
    };
}

/// Custom macro to bind an OSTree GObject type.
macro_rules! bind_ostree_obj {
    ($w:ident) => {
        paste! {
            #[repr(transparent)]
            pub struct [<FFIOstree $w>](ostree_sys::[<Ostree $w>]);

            unsafe impl ExternType for [<FFIOstree $w>] {
                type Id = type_id!(rpmostreecxx::[<Ostree $w>]);
                type Kind = cxx::kind::Trivial;
            }
            impl_wrap!([<FFIOstree $w>], ostree::$w);
        }
    };
}

bind_ostree_obj!(Sysroot);
bind_ostree_obj!(Repo);
bind_ostree_obj!(Deployment);
