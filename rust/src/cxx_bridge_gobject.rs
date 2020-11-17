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

pub trait FFIGObjectWrapper {
    type Wrapper;

    fn gobj_wrap(&mut self) -> Self::Wrapper;
}

macro_rules! wrap {
    ($w:ident, $bound:path) => {
        impl FFIGObjectWrapper for $w {
            type Wrapper = $bound;
            fn gobj_wrap(&mut self) -> Self::Wrapper {
                unsafe { glib::translate::from_glib_none(&mut self.0 as *mut _) }
            }
        }
    }
}

#[repr(transparent)]
pub struct FFIOstreeRepo(ostree_sys::OstreeRepo);

unsafe impl ExternType for FFIOstreeRepo {
    type Id = type_id!("rpmostreecxx::OstreeRepo");
    type Kind = cxx::kind::Trivial;
}
wrap!(FFIOstreeRepo, ostree::Repo);

#[repr(transparent)]
pub struct FFIOstreeDeployment(ostree_sys::OstreeDeployment);

unsafe impl ExternType for FFIOstreeDeployment {
    type Id = type_id!("rpmostreecxx::OstreeDeployment");
    type Kind = cxx::kind::Trivial;
}
wrap!(FFIOstreeDeployment, ostree::Deployment);

pub struct FFIGString(glib::GString);
unsafe impl ExternType for FFIGString {
    type Id = type_id!("rpmostreecxx::GString");
    type Kind = cxx::kind::Trivial;
}