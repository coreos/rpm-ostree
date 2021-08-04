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
use paste::paste;
use std::pin::Pin;

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
    fn gobj_wrap(&mut self) -> Self::Wrapper;
}

pub trait FFIGObjectReWrap<'a> {
    type ReWrapped;

    /// Convert a glib-rs wrapper object into a Pin pointer
    /// to our FFI newtype.  This is necessary to call
    /// cxx-rs wrapped functions from Rust.
    fn gobj_rewrap(&'a self) -> Pin<&'a mut Self::ReWrapped>;
}

/// Implement FFIGObjectWrapper given a pair of wrapper type
/// and sys type.
macro_rules! impl_wrap {
    ($w:ident, $bound:path, $sys:path) => {
        impl FFIGObjectWrapper for $w {
            type Wrapper = $bound;
            fn gobj_wrap(&mut self) -> Self::Wrapper {
                unsafe { glib::translate::from_glib_none(&mut self.0 as *mut _) }
            }
        }
        impl<'a> FFIGObjectReWrap<'a> for $bound {
            type ReWrapped = $w;
            fn gobj_rewrap(&'a self) -> Pin<&'a mut Self::ReWrapped> {
                // Access the underlying raw pointer behind the glib-rs
                // newtype wrapper, e.g. `ostree_sys::OstreeRepo`.
                let p: *const $sys = self.to_glib_none().0;
                // Safety: Pin<T> is a #[repr(transparent)] newtype wrapper
                // around our #[repr(transparent)] FFI newtype wrapper which
                // for the glib-rs newtype wrapper, which finally holds the real
                // raw pointer.  Phew!
                // In other words: Pin(FFINewType(GlibRs(RawPointer)))
                // Here we're just powering through those layers of wrappers to
                // convert the raw pointer.  See also https://internals.rust-lang.org/t/pre-rfc-v2-safe-transmute/11431
                //
                // However, since what we're handing out is a raw pointer,
                // we ensure that the lifetime of our return value is tied to
                // that of the glib-rs wrapper (which holds a GObject strong reference),
                // which ensures the value isn't freed.
                unsafe { std::mem::transmute(p) }
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
    ostree_sys,
    [Deployment, Repo, RepoTransactionStats, Sysroot]
);
cxxrs_bind!(G, glib, glib::gobject_ffi, [Object]);
cxxrs_bind!(G, gio, gio::ffi, [Cancellable, DBusConnection, FileInfo]);
cxxrs_bind!(G, glib, glib_sys, [KeyFile, Variant, VariantDict]);

// An error type helper; separate from the GObject bridging
mod err {
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
            Self(format!("{}: {}", context.to_string(), self))
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
        let cancellable = gio::NONE_CANCELLABLE;
        let td = tempfile::tempdir()?;
        let p = td.path().join("repo");
        let r = ostree::Repo::new_for_path(&p);
        r.create(ostree::RepoMode::Archive, cancellable)?;
        let fd = r.dfd();
        assert_eq!(
            fd,
            crate::ffi::testutil_validate_cxxrs_passthrough(r.gobj_rewrap())
        );
        Ok(())
    }
}
