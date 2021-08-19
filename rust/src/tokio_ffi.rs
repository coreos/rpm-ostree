//! Helpers to bridge tokio to C++

// SPDX-License-Identifier: Apache-2.0 OR MIT

pub(crate) struct TokioHandle(tokio::runtime::Handle);
pub(crate) struct TokioEnterGuard<'a>(tokio::runtime::EnterGuard<'a>);

pub(crate) fn tokio_handle_get() -> Box<TokioHandle> {
    Box::new(TokioHandle(tokio::runtime::Handle::current()))
}

impl TokioHandle {
    pub(crate) fn enter(&self) -> Box<TokioEnterGuard> {
        Box::new(TokioEnterGuard(self.0.enter()))
    }
}
