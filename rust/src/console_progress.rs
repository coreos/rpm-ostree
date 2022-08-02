//! A wrapper around the `indicatif` crate that maintains
//! stateful drawing to a TTY console.  Note that much
//! of rpm-ostree needs to work as both a DBus daemon
//! under systemd, and a direct process in a podman/Kube
//! container.  So there's a wrapping/indirection layer
//! which currently lives in C++ in rpmostree-output.h.

/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

use indicatif::{ProgressBar, ProgressDrawTarget, ProgressStyle};
use once_cell::sync::Lazy;
use std::sync::Mutex;
use std::sync::MutexGuard;

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

static PROGRESS: Lazy<Mutex<Option<ProgressState>>> = Lazy::new(|| Mutex::new(None));

impl ProgressState {
    /// Create a new progress bar.  Should really only be stored
    /// in the PROGRESS static ref.
    fn new<M: Into<String>>(msg: M, ptype: ProgressType) -> Self {
        use std::fmt::Write;
        let msg = msg.into();
        let target = ProgressDrawTarget::stdout();
        let style = ProgressStyle::default_bar();
        let pb = match ptype {
            ProgressType::Task => {
                let pb = ProgressBar::new_spinner();
                pb.set_style(style.template("{spinner} {prefix} {msg}").unwrap());
                pb.enable_steady_tick(std::time::Duration::from_millis(200));
                pb
            }
            ProgressType::NItems(n) => {
                let pb = ProgressBar::new(n);
                let width = n_digits(n);
                // Our width is dynamic, so format the format string with it
                let mut fmt = String::new();
                fmt.push_str("{spinner} {prefix} {pos:>");
                write!(fmt, "{width}").unwrap();
                fmt.push_str("}/{len:");
                write!(fmt, "{width}").unwrap();
                fmt.push_str("} [{bar:20}] ({eta}) {msg}");
                pb.set_style(style.template(&fmt).unwrap());
                pb
            }
            ProgressType::Percent => {
                let pb = ProgressBar::new(100);
                pb.set_style(
                    style
                        .template("{spinner} {prefix} {pos:>3}% [{bar:20}] ({eta}) {msg}")
                        .unwrap(),
                );
                pb
            }
        };
        let is_hidden = target.is_hidden();
        if is_hidden {
            print!("{}...", msg);
        } else {
            let prefix = match ptype {
                ProgressType::Task => format!("{}...", msg),
                _ => msg.clone(),
            };
            pb.set_prefix(prefix);
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
        self.bar.set_prefix(msg.clone());
        self.message = msg;
    }

    /// Change the "sub message" which is the indicatif `{message}`. This text
    /// appears after everything else - it's meant for text that changes width
    /// often (otherwise the progress bar would bounce around).
    fn set_sub_message<T: AsRef<str>>(&self, msg: Option<T>) {
        if let Some(ref sub_message) = msg {
            self.bar.set_message(sub_message.as_ref().to_string())
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

fn assert_empty(m: &MutexGuard<Option<ProgressState>>) {
    if let Some(ref state) = **m {
        panic!("Overwriting task: \"{}\"", state.message)
    }
}

// TODO cxx-rs Option<T>
fn optional_str(s: &str) -> Option<&str> {
    Some(s).filter(|s| !s.is_empty())
}

// NOTE!  These APIs are essentially just a *backend* of the rpmostree-output.h
// API.
pub(crate) fn console_progress_begin_task(msg: &str) {
    let mut lock = PROGRESS.lock().unwrap();
    assert_empty(&lock);
    *lock = Some(ProgressState::new(msg, ProgressType::Task));
}

pub(crate) fn console_progress_begin_n_items(msg: &str, n: u64) {
    let mut lock = PROGRESS.lock().unwrap();
    assert_empty(&lock);
    *lock = Some(ProgressState::new(msg, ProgressType::NItems(n as u64)));
}

pub(crate) fn console_progress_begin_percent(msg: &str) {
    let mut lock = PROGRESS.lock().unwrap();
    assert_empty(&lock);
    *lock = Some(ProgressState::new(msg, ProgressType::Percent));
}

pub(crate) fn console_progress_set_message(msg: &str) {
    let mut lock = PROGRESS.lock().unwrap();
    let state = lock.as_mut().expect("progress to set message");
    state.set_message(msg);
}

pub(crate) fn console_progress_set_sub_message(msg: &str) {
    let msg = optional_str(msg);
    let mut lock = PROGRESS.lock().unwrap();
    let state = lock.as_mut().expect("progress sub-msg update");
    state.set_sub_message(msg);
}

pub(crate) fn console_progress_update(n: u64) {
    let lock = PROGRESS.lock().unwrap();
    let state = lock.as_ref().expect("progress to update");
    state.update(n);
}

pub(crate) fn console_progress_end(suffix: &str) {
    let suffix = optional_str(suffix);
    let mut lock = PROGRESS.lock().unwrap();
    let state = lock.take().expect("progress to end");
    state.end(suffix);
}
