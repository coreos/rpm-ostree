//! Wrappers that bridge the world of cxx.rs and GObject-introspect based crate bindings.
//! See https://github.com/dtolnay/cxx/issues/544
//! While cxx.rs supports including externally-bound types (like ostree::Repo),
//! two things make this more complicated.  First, cxx.rs requires implementing
//! a trait, and due to the orphan rule we can't implement foreign traits on
//! foreign types.  Second, what we *actually* want on the Rust side isn't
//! the *_sys type (e.g. ostree_sys::OstreeRepo) but the `ostree::Repo` type.
//! So for now, we define a `FFIGObjectWrapper` trait that helps with this.
//! In the future though hopefully cxx.rs improves this situation.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use cxx::{type_id, ExternType};
use glib::translate::ToGlibPtr;
use ostree_ext::{gio, glib, ostree};
use pastey::paste;

/// Map an empty string to a `None`.
pub(crate) fn opt_string(input: &str) -> Option<&str> {
    // TODO(lucab): drop this polyfill once cxx-rs starts supporting Option<_>.
    Some(input).filter(|s| !s.is_empty())
}

/// A custom trait used to translate a *_sys C type wrapper
/// to its GObject version.
pub trait FFIGObjectWrapper {
    type Wrapper;

    /// Use this function in Rust code that accepts glib-rs
    /// objects passed via cxx-rs to synthesize the expected glib-rs
    /// wrapper type.
    fn gobj_wrap(&self) -> Self::Wrapper;

    /// Convert a borrowed cxx-rs type back into a borrowed version
    /// of the glib-rs type.
    fn glib_reborrow(&self) -> glib::translate::Borrowed<Self::Wrapper>;
}

pub trait FFIGObjectReWrap<'a> {
    type ReWrapped;

    /// Convert a glib-rs wrapper object borrowed cxx-rs type.
    fn reborrow_cxx(&'a self) -> &'a Self::ReWrapped;
}

/// Implement FFIGObjectWrapper given a pair of wrapper type
/// and sys type.
macro_rules! impl_wrap {
    ($w:ident, $bound:path, $sys:path) => {
        impl FFIGObjectWrapper for $w {
            type Wrapper = $bound;
            fn gobj_wrap(&self) -> Self::Wrapper {
                unsafe { glib::translate::from_glib_none(&self.0 as *const _) }
            }

            fn glib_reborrow(&self) -> glib::translate::Borrowed<Self::Wrapper> {
                unsafe { glib::translate::from_glib_borrow(&self.0 as *const _) }
            }
        }
        impl<'a> FFIGObjectReWrap<'a> for $bound {
            type ReWrapped = $w;

            fn reborrow_cxx(&'a self) -> &'a Self::ReWrapped {
                let p: *const $sys = self.to_glib_none().0;
                let p = p as *const Self::ReWrapped;
                unsafe { &*p }
            }
        }
    };
}

/// Custom macro to bind gtk-rs bridged types.
macro_rules! cxxrs_bind {
    ($ns:ident, $lowerns:ident, $sys:path, [ $( $i:ident ),* ]) => {
        paste! {
            $(
                #[repr(transparent)]
                #[derive(Debug)]
                pub struct [<FFI $ns $i>](pub(crate) $sys::[<$ns $i>]);

                unsafe impl ExternType for [<FFI $ns $i>] {
                    type Id = type_id!(rpmostreecxx::[<$ns $i>]);
                    type Kind = cxx::kind::Trivial;
                }
                impl_wrap!([<FFI $ns $i>], $lowerns::$i, $sys::[<$ns $i>]);
            )*
        }
    };
}

// When extending this list, also update rpmostree-cxxrs-prelude.h and lib.rs
// This macro is special to ostree types currently.
cxxrs_bind!(
    Ostree,
    ostree,
    ostree::ffi,
    [Deployment, Repo, RepoTransactionStats, SePolicy, Sysroot]
);
cxxrs_bind!(G, glib, glib::gobject_ffi, [Object]);
cxxrs_bind!(G, gio, gio::ffi, [Cancellable, DBusConnection, FileInfo]);
cxxrs_bind!(G, glib, glib::ffi, [KeyFile, Variant, VariantDict]);

/// Error type helpers; separate from the GObject bridging.
///
/// # Use CxxResult in cxx.rs-bridged APIs (i.e. in `lib.rs`)
///
/// The `CxxResult<T>` type is necessarily primarily because
/// we want to change the default error formatting.  It
/// implements `From<T>` for many other error types, such as
/// `anyhow::Error` and `glib::Error`, so the default conversion
/// from `?` should work.
mod err {
    use ostree_ext::glib;
    use std::error::Error as StdError;
    use std::fmt::Display;
    use std::io::Error as IoError;

    // See the documentation for CxxResult
    #[derive(Debug)]
    pub struct CxxError(String);

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

    impl From<String> for CxxError {
        fn from(v: String) -> Self {
            Self(v)
        }
    }

    impl From<Box<dyn std::error::Error + Send + Sync>> for CxxError {
        fn from(e: Box<dyn std::error::Error + Send + Sync>) -> Self {
            Self(e.to_string())
        }
    }

    impl From<cxx::Exception> for CxxError {
        fn from(v: cxx::Exception) -> Self {
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

    impl CxxError {
        /// Prefix an error message with `<context>: `.  See
        /// https://docs.rs/anyhow/1.0.38/anyhow/struct.Error.html#method.context
        /// This is necessary for use with the `fn-error-context` crate.
        pub(crate) fn context<C>(self, context: C) -> Self
        where
            C: Display + Send + Sync + 'static,
        {
            Self(format!("{context}: {self}"))
        }
    }

    // Use this on exit from Rust functions that return to C++ (bridged via cxx-rs).
    // This is a workaround for https://github.com/dtolnay/cxx/issues/290#issuecomment-756432907
    // which is that cxx-rs only shows the first entry in the cause chain.
    pub(crate) type CxxResult<T> = std::result::Result<T, CxxError>;

    #[cfg(test)]
    mod tests {
        use super::*;
        use fn_error_context::context;

        #[test]
        fn throwchain() {
            #[context("outer")]
            fn outer() -> CxxResult<()> {
                #[context("inner")]
                fn inner() -> anyhow::Result<()> {
                    anyhow::bail!("oops")
                }
                Ok(inner()?)
            }

            assert_eq!(format!("{}", outer().err().unwrap()), "outer: inner: oops")
        }
    }
}
pub(crate) use err::*;

#[cfg(test)]
mod test {
    use super::*;
    use anyhow::Result;

    #[test]
    fn passthrough() -> Result<()> {
        let cancellable = gio::Cancellable::NONE;
        let td = tempfile::tempdir()?;
        let p = td.path().join("repo");
        let r = ostree::Repo::new_for_path(&p);
        r.create(ostree::RepoMode::Archive, cancellable)?;
        let fd = r.dfd();
        assert_eq!(
            fd,
            crate::ffi::testutil_validate_cxxrs_passthrough(r.reborrow_cxx())
        );
        Ok(())
    }
}
