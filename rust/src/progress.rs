//! Rust convenience APIs over our rpmostree-output.h
//! progress/output APIs.

// SPDX-License-Identifier: Apache-2.0 OR MIT

/// Call the provided function, while displaying a "task progress"
/// message.
pub(crate) fn progress_task<F, T>(msg: &str, f: F) -> T
where
    F: FnOnce() -> T,
{
    // Drop will end the task
    let _task = crate::ffi::progress_begin_task(msg);
    f()
}
