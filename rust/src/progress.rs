/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use indicatif::{ProgressBar, ProgressDrawTarget, ProgressStyle};
use lazy_static::lazy_static;
use std::borrow::Cow;
use std::sync::Mutex;

#[derive(PartialEq)]
enum ProgressType {
    Task,
    NItems(u64),
    Percent,
}

/// A wrapper around indicatif's ProgressBar with some extra state.
struct ProgressState {
    bar: ProgressBar,
    // In some cases we still want to print things even if stdout
    // isn't a tty; this helps us know that.
    is_hidden: bool,
    // We have a high level concept of progress bar types.
    ptype: ProgressType,
    // indicatif doesn't expose an API to retrieve the message used,
    // but we want to print "Frobnicating...done".  So we keep around
    // the original message and use it sometimes.  Also, to add confusion
    // this `message` is really the `prefix` in the format string.
    message: String,
}

// We only have one stdout, so we can really only print one progress
// bar at a time.  I understand why indicatif didn't want to commit
// to having static data, but still.
lazy_static! {
    static ref PROGRESS: Mutex<Option<ProgressState>> = Mutex::new(None);
}

impl ProgressState {
    /// Create a new progress bar.  Should really only be stored
    /// in the PROGRESS static ref.
    fn new<M: Into<String>>(msg: M, ptype: ProgressType) -> Self {
        let msg = msg.into();
        let target = ProgressDrawTarget::stdout();
        let style = ProgressStyle::default_bar();
        let pb = match ptype {
            ProgressType::Task => {
                let pb = ProgressBar::new_spinner();
                pb.set_style(style.template("{spinner} {prefix} {msg}"));
                pb.enable_steady_tick(200);
                pb
            }
            ProgressType::NItems(n) => {
                let pb = ProgressBar::new(n);
                let width = n_digits(n);
                // Our width is static, so format the format string with it
                let fmt = format!("{{spinner}} {{prefix}} {{pos:>{width}$}}/{{len:width$}} [{{bar:20}}] ({{eta}}) {{msg}}",
                                  width = width);
                pb.set_style(style.template(&fmt));
                pb
            }
            ProgressType::Percent => {
                let pb = ProgressBar::new(100);
                pb.set_style(
                    style.template("{spinner} {prefix} {pos:>3}% [{bar:20}] ({eta}) {msg}"),
                );
                pb
            }
        };
        let is_hidden = target.is_hidden();
        if is_hidden {
            print!("{}...", msg);
        } else {
            let msg = match ptype {
                ProgressType::Task => Cow::Owned(format!("{}...", msg)),
                _ => Cow::Borrowed(&msg),
            };
            pb.set_prefix(&msg);
        }
        Self {
            bar: pb,
            is_hidden,
            ptype,
            message: msg,
        }
    }

    /// Change the "message" which is actually the indicatif `{prefix}`. This
    /// text appears near the start of the progress bar.
    fn set_message<M: Into<String>>(&mut self, msg: M) {
        let msg = msg.into();
        self.bar.set_prefix(&msg);
        self.message = msg;
    }

    /// Change the "sub message" which is the indicatif `{message}`. This text
    /// appears after everything else - it's meant for text that changes width
    /// often (otherwise the progress bar would bounce around).
    fn set_sub_message<T: AsRef<str>>(&self, msg: Option<T>) {
        if let Some(ref sub_message) = msg {
            self.bar.set_message(sub_message.as_ref())
        } else {
            self.bar.set_message("");
        }
    }

    /// For a percent or nitems progress, set the progress state.
    fn update(&self, n: u64) {
        assert!(!(self.ptype == ProgressType::Task));
        self.bar.set_position(n);
    }

    /// Clear the progress bar and print a completion message even on non-ttys.
    fn end<T: AsRef<str>>(&self, suffix: Option<T>) {
        self.bar.finish_and_clear();
        let suffix = suffix.as_ref().map(|s| s.as_ref()).unwrap_or("done");
        if self.is_hidden {
            println!("{}", suffix);
        } else {
            println!("{}... {}", self.message, suffix);
        }
    }
}

/// Compute the maximum number of digits needed to represent an integer when
/// formatted as decimal.
fn n_digits(n: u64) -> u32 {
    let mut width = 1;
    let mut n = n;
    while n >= 10 {
        width += 1;
        n /= 10;
    }
    width
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_n_digits() {
        assert_eq!(n_digits(0), 1);
        assert_eq!(n_digits(9), 1);
        assert_eq!(n_digits(10), 2);
        assert_eq!(n_digits(98), 2);
        assert_eq!(n_digits(100), 3);
        assert_eq!(n_digits(177), 3);
        assert_eq!(n_digits(123798), 6);
        assert_eq!(n_digits(7123798), 7);
    }
}

mod ffi {
    use super::*;
    use glib::translate::*;
    use libc;
    use std::sync::MutexGuard;

    fn assert_empty(m: &MutexGuard<Option<ProgressState>>) {
        if let Some(ref state) = **m {
            panic!("Overwriting task: \"{}\"", state.message)
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_progress_begin_task(msg: *const libc::c_char) {
        let msg: String = unsafe { from_glib_none(msg) };
        let mut lock = PROGRESS.lock().unwrap();
        assert_empty(&lock);
        *lock = Some(ProgressState::new(msg, ProgressType::Task));
    }

    #[no_mangle]
    pub extern "C" fn ror_progress_begin_n_items(msg: *const libc::c_char, n: libc::c_int) {
        let msg: String = unsafe { from_glib_none(msg) };
        let mut lock = PROGRESS.lock().unwrap();
        assert_empty(&lock);
        *lock = Some(ProgressState::new(msg, ProgressType::NItems(n as u64)));
    }

    #[no_mangle]
    pub extern "C" fn ror_progress_begin_percent(msg: *const libc::c_char) {
        let msg: String = unsafe { from_glib_none(msg) };

        let mut lock = PROGRESS.lock().unwrap();
        assert_empty(&lock);
        *lock = Some(ProgressState::new(msg, ProgressType::Percent));
    }

    #[no_mangle]
    pub extern "C" fn ror_progress_set_message(msg: *const libc::c_char) {
        let msg: String = unsafe { from_glib_none(msg) };
        let mut lock = PROGRESS.lock().unwrap();
        let state = lock.as_mut().expect("progress to update");
        state.set_message(msg);
    }

    #[no_mangle]
    pub extern "C" fn ror_progress_set_sub_message(msg: *const libc::c_char) {
        let msg: Option<String> = unsafe { from_glib_none(msg) };
        let mut lock = PROGRESS.lock().unwrap();
        let state = lock.as_mut().expect("progress to update");
        state.set_sub_message(msg);
    }

    #[no_mangle]
    pub extern "C" fn ror_progress_update(n: libc::c_int) {
        let lock = PROGRESS.lock().unwrap();
        let state = lock.as_ref().expect("progress to update");
        state.update(n as u64);
    }

    #[no_mangle]
    pub extern "C" fn ror_progress_end(suffix: *const libc::c_char) {
        let suffix: Option<String> = unsafe { from_glib_none(suffix) };
        let mut lock = PROGRESS.lock().unwrap();
        let state = lock.take().expect("progress to end");
        state.end(suffix);
    }
}
pub use self::ffi::*;
