//! Logic for unpacking and importing an RPM.
//!
//! The design here is to reuse libarchive's RPM support for most of it.
//! We do however need to look at file capabilities, which are part of the header.
//! Hence we end up with two file descriptors open.

// SPDX-License-Identifier: Apache-2.0 OR MIT

/// Whether absolute `path` is allowed in OSTree content.
///
/// When we do a unified core, we'll likely need to add /boot to pick up
/// kernels here at least.  This is intended short term to address
/// https://github.com/projectatomic/rpm-ostree/issues/233
pub fn path_is_ostree_compliant(path: &str) -> bool {
    if matches!(path, "/" | "/usr" | "/bin" | "/sbin" | "/lib" | "/lib64") {
        return true;
    }

    if path.starts_with("/bin/")
        || path.starts_with("/sbin/")
        || path.starts_with("/lib/")
        || path.starts_with("/lib64/")
    {
        return true;
    }

    if path.starts_with("/usr/") && !path.starts_with("/usr/local") {
        return true;
    }

    false
}

/// Whether absolute `path` belongs to `/opt` hierarchy.
pub fn path_is_in_opt(path: &str) -> bool {
    path == "/opt" || path.starts_with("/opt/")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_path_is_compliant() {
        let ostree_cases = &["/", "/usr", "/usr/share", "/bin/foo"];
        for entry in ostree_cases {
            assert_eq!(path_is_ostree_compliant(entry), true, "{}", entry);
            assert_eq!(path_is_in_opt(entry), false, "{}", entry);
        }

        let opt_cases = &["/opt", "/opt/misc"];
        for entry in opt_cases {
            assert_eq!(path_is_ostree_compliant(entry), false, "{}", entry);
            assert_eq!(path_is_in_opt(entry), true, "{}", entry);
        }

        let denied_cases = &["/var", "/etc", "/var/run", "/usr/local", "", "./", "usr/"];
        for entry in denied_cases {
            assert_eq!(path_is_ostree_compliant(entry), false, "{}", entry);
            assert_eq!(path_is_in_opt(entry), false, "{}", entry);
        }
    }
}
