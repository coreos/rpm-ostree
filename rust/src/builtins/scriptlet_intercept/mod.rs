//! CLI handler for `rpm-ostree scriplet-intercept`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

mod groupadd;
use anyhow::{bail, Result};

/// Entrypoint for `rpm-ostree scriplet-intercept`.
pub fn entrypoint(args: &[&str]) -> Result<()> {
    // Here we expect arguments that look like
    // `rpm-ostree scriptlet-intercept <command> -- <rest>`
    if args.len() < 4 || args[3] != "--" {
        bail!("Invalid arguments");
    }

    let orig_command = args[2];
    let rest = &args[4..];
    match orig_command {
        "groupadd" => groupadd::entrypoint(rest),
        x => bail!("Unable to intercept command '{}'", x),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_entrypoint_args() {
        // Short-circuit groupadd logic, this test is only meant to check CLI parsing.
        let _guard = fail::FailScenario::setup();
        fail::cfg("intercept_groupadd_ok", "return").unwrap();

        let err_cases = [
            vec![],
            vec!["rpm-ostree", "install"],
            vec!["rpm-ostree", "scriptlet-intercept", "groupadd"],
            vec!["rpm-ostree", "scriptlet-intercept", "foo", "--"],
        ];
        for input in &err_cases {
            entrypoint(input).unwrap_err();
        }

        let ok_cases = [vec!["rpm-ostree", "scriptlet-intercept", "groupadd", "--"]];
        for input in &ok_cases {
            entrypoint(input).unwrap();
        }
    }
}
