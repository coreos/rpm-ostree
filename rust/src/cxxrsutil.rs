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

/// Map an empty string to a `None`.
pub(crate) fn opt_string(input: &str) -> Option<&str> {
    // TODO(lucab): drop this polyfill once cxx-rs starts supporting Option<_>.
    Some(input).filter(|s| !s.is_empty())
}

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

// When extending this list, also update rpmostree-cxxrs-prelude.h and lib.rs
// This macro is special to ostree types currently.
bind_ostree_obj!(Sysroot);
bind_ostree_obj!(Repo);
bind_ostree_obj!(Deployment);

// List of non-ostree types we want to bind; if you need to extend this list
// try to instead create a bind_gio_obj!() macro or so.
#[repr(transparent)]
pub struct FFIGCancellable(gio_sys::GCancellable);

unsafe impl ExternType for FFIGCancellable {
    type Id = type_id!(rpmostreecxx::GCancellable);
    type Kind = cxx::kind::Trivial;
}
impl_wrap!(FFIGCancellable, gio::Cancellable);

// An error type helper; separate from the GObject bridging
mod err {
    use std::error::Error as StdError;
    use std::fmt::Display;
    use std::io::Error as IoError;

    // See the documentation for CxxResult
    #[derive(Debug)]
    pub(crate) struct CxxError(String);

    impl Display for CxxError {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            f.write_str(self.0.as_str())
        }
    }

    impl StdError for CxxError {
        fn source(&self) -> Option<&(dyn StdError + 'static)> {
            None
        }

        fn description(&self) -> &str {
            "description() is deprecated; use Display"
        }

        fn cause(&self) -> Option<&dyn StdError> {
            None
        }
    }

    impl From<anyhow::Error> for CxxError {
        fn from(v: anyhow::Error) -> Self {
            Self(format!("{:#}", v))
        }
    }

    impl From<IoError> for CxxError {
        fn from(v: IoError) -> Self {
            Self(format!("{}", v))
        }
    }

    impl From<nix::Error> for CxxError {
        fn from(v: nix::Error) -> Self {
            Self(format!("{}", v))
        }
    }

    impl From<glib::error::Error> for CxxError {
        fn from(v: glib::error::Error) -> Self {
            Self(format!("{}", v))
        }
    }

    // Use this on exit from Rust functions that return to C++ (bridged via cxx-rs).
    // This is a workaround for https://github.com/dtolnay/cxx/issues/290#issuecomment-756432907
    // which is that cxx-rs only shows the first entry in the cause chain.
    pub(crate) type CxxResult<T> = std::result::Result<T, CxxError>;

    #[cfg(test)]
    mod tests {
        use super::*;
        #[test]
        fn throwchain() {
            use anyhow::Context;
            fn outer() -> CxxResult<()> {
                fn inner() -> anyhow::Result<()> {
                    anyhow::bail!("inner")
                }
                Ok(inner().context("Failed in outer")?)
            }
            assert_eq!(
                format!("{}", outer().err().unwrap()),
                "Failed in outer: inner"
            )
        }
    }
}
pub(crate) use err::*;
