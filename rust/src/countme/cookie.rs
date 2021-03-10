// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{bail, Result};
use chrono::prelude::*;
use openat_ext::OpenatDirExt;
use serde::{Deserialize, Serialize};
use std::io::Read;

/// State directory used to store the countme cookie
const STATE_DIR: &str = "/var/lib/rpm-ostree-countme";
/// Cookie file name
const COUNTME_COOKIE: &str = "countme";

/// Width of the sliding time window (in seconds): 1 week
const COUNTME_WINDOW: i64 = 7 * 24 * 60 * 60;
/// Starting point of the sliding time window relative to the UNIX epoch:
/// Monday (1970-01-05 00:00:00 UTC)
/// Allows for aligning the window with a specific weekday
const COUNTME_OFFSET: i64 = 345600;

/// Cookie v0 JSON file format
///
/// The values stored in this cookie are the same as the ones used in the DNF
/// Count Me Cookie with the exception of the COUNTME_BUDGET which is not
/// included as this implementation directly queries all repo metalinks once:
///   - epoch: the timestamp of the first ever counted window
///   - window: the timestamp of the last successfully counted window
///
/// See the libdnf implementation at:
/// https://github.com/rpm-software-management/libdnf/blob/95b88b141a3f97feb94eadb2480f6857b6d1fcae/libdnf/repo/Repo.cpp#L1038
///
/// This cookie is marked as version 0 similarly to how libdnf defines a version
/// for their cookie to simplify potential future updates but no additional code
/// has been added yet to handle multiple versions as we do not need it yet.
#[derive(Serialize, Deserialize, Debug, PartialEq)]
struct CookieV0 {
    epoch: i64,
    window: i64,
}

impl CookieV0 {
    /// Try to parse a string as a v0 cookie
    fn new(string: &str) -> Result<Self> {
        let c: CookieV0 = serde_json::from_str(string)?;
        if c.epoch <= 0 {
            bail!("Invalid value for epoch: '{}'", c.epoch);
        }
        if c.window <= 0 {
            bail!("Invalid value for window: '{}'", c.window);
        }
        Ok(c)
    }
}

/// Internal representation of the values loaded from the versioned cookie
/// format.
#[derive(Clone, Debug)]
pub struct Cookie {
    epoch: i64,
    window: i64,
    now: i64,
}

impl Cookie {
    /// Load cookie timestamps from persistent directory if it exists.
    /// Returns an error if we can not read an existing cookie
    /// Returns a default cookie (counting never started) in all other cases.
    pub fn new() -> Result<Self> {
        // Start default window at COUNTME_OFFSET to avoid negative values
        let now = Utc::now().timestamp();
        let mut c = Cookie {
            epoch: now,
            window: COUNTME_OFFSET,
            now,
        };

        // Read cookie values from the state persisted on the filesystem
        let mut content = String::new();
        match openat::Dir::open(STATE_DIR)?.open_file_optional(COUNTME_COOKIE)? {
            Some(mut f) => f.read_to_string(&mut content)?,
            None => return Ok(c),
        };
        match CookieV0::new(&content) {
            Err(e) => eprintln!("Ignoring existing cookie: {}", e),
            Ok(cookie_v0) => {
                c.epoch = cookie_v0.epoch;
                c.window = cookie_v0.window;
            }
        };

        // Reset epoch if it is in the future
        if c.epoch > c.now {
            c.epoch = c.now;
        }

        Ok(c)
    }

    /// Compute the Count Me window for the epoch stored in the cookie
    fn epoch_window(&self) -> i64 {
        (self.epoch - COUNTME_OFFSET) / COUNTME_WINDOW
    }

    /// Compute the Count Me window for the previous window stored in the cookie
    fn previous_window(&self) -> i64 {
        (self.window - COUNTME_OFFSET) / COUNTME_WINDOW
    }

    /// Compute the Count Me window for current time
    fn current_window(&self) -> i64 {
        (self.now - COUNTME_OFFSET) / COUNTME_WINDOW
    }

    /// Are we still in the same window as the one we loaded from the cookie?
    pub fn existing_window(&self) -> bool {
        self.current_window() <= self.previous_window()
    }

    // Count Me window logic
    // https://dnf.readthedocs.io/en/latest/conf_ref.html?highlight=countme#options-for-both-main-and-repo
    // https://github.com/rpm-software-management/libdnf/blob/95b88b141a3f97feb94eadb2480f6857b6d1fcae/libdnf/repo/Repo.cpp#L1038
    pub fn get_window_counter(&self) -> i64 {
        // Start counting windows from 1
        let mut counter = self.current_window() - self.epoch_window() + 1;
        if counter <= 0 {
            // Should never happen as this is checked at creation time
            counter = 1;
        }
        match counter {
            1 => 1,
            2..=4 => 2,
            5..=24 => 3,
            _ => 4, // >= 25
        }
    }

    /// Update cookie timestamps that are persisted on disk
    pub fn persist(&self) -> Result<()> {
        let cookie = CookieV0 {
            epoch: self.epoch,
            window: self.now,
        };
        openat::Dir::open(STATE_DIR)?.write_file_with(COUNTME_COOKIE, 0o644, |w| -> Result<_> {
            Ok(serde_json::to_writer(w, &cookie)?)
        })?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_cookie_v0() {
        let line = r#"{ "epoch": 1607706068, "window": 1607706068 }"#;
        assert_eq!(
            CookieV0::new(line).unwrap(),
            CookieV0 {
                epoch: 1607706068,
                window: 1607706068,
            }
        );

        let line = r#"{ "epoch": 1600006068, "window": 1607706068 }"#;
        assert_eq!(
            CookieV0::new(line).unwrap(),
            CookieV0 {
                epoch: 1600006068,
                window: 1607706068,
            }
        );

        let line = r#"{ "epoch": -1, "window": 1607706068 }"#;
        assert!(CookieV0::new(line).is_err());

        let line = r#"{ "epoch": "1600006068", window: -1 }"#;
        assert!(CookieV0::new(line).is_err());
    }

    #[test]
    fn test_window_counter() {
        // Invalid cookie (epoch is in the future)
        let cookie = Cookie {
            epoch: 1607703629,      // Friday, December 11, 2020 4:20:29 PM
            window: COUNTME_OFFSET, // Monday, January 5, 1970 12:00:00 AM
            now: 976724429,         // Wednesday, December 13, 2000 4:20:29 PM
        };
        assert_eq!(cookie.get_window_counter(), 1);

        // Fresh cookie (epoch = now)
        let cookie = Cookie {
            epoch: 1607703629,      // Friday, December 11, 2020 4:20:29 PM
            window: COUNTME_OFFSET, // Monday, January 5, 1970 12:00:00 AM
            now: 1607703629,        // Friday, December 11, 2020 4:20:29 PM
        };
        assert_eq!(cookie.get_window_counter(), 1);

        // First week
        let cookie = Cookie {
            epoch: 1607703629,      // Friday, December 11, 2020 4:20:29 PM
            window: COUNTME_OFFSET, // Monday, January 5, 1970 12:00:00 AM
            now: 1607876429,        // Sunday, December 13, 2020 4:20:29 PM
        };
        assert_eq!(cookie.get_window_counter(), 1);

        // First month
        let cookie = Cookie {
            epoch: 1607703629,      // Friday, December 11, 2020 4:20:29 PM
            window: COUNTME_OFFSET, // Monday, January 5, 1970 12:00:00 AM
            now: 1608135629,        // Wednesday, December 16, 2020 4:20:29 PM
        };
        assert_eq!(cookie.get_window_counter(), 2);

        // First six months
        let cookie = Cookie {
            epoch: 1607703629,      // Friday, December 11, 2020 4:20:29 PM
            window: COUNTME_OFFSET, // Monday, January 5, 1970 12:00:00 AM
            now: 1613838029,        // Saturday, February 20, 2021 4:20:29 PM
        };
        assert_eq!(cookie.get_window_counter(), 3);

        // More than six months
        let cookie = Cookie {
            epoch: 1607703629,      // Friday, December 11, 2020 4:20:29 PM
            window: COUNTME_OFFSET, // Monday, January 5, 1970 12:00:00 AM
            now: 1647793229,        // Sunday, March 20, 2022 4:20:29 PM
        };
        assert_eq!(cookie.get_window_counter(), 4);
    }
}
