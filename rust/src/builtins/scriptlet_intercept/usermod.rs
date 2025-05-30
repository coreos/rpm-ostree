//! CLI handler for intercepted `usermod`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use super::common::{self, SYSUSERS_DIR};
use anyhow::{anyhow, Context, Result};
use cap_std::fs::{Dir, Permissions, PermissionsExt};
use cap_std_ext::prelude::CapStdExtDirExt;
use clap::{Arg, ArgAction, Command};
use fn_error_context::context;
use std::io::Write;

/// Entrypoint for (the rpm-ostree implementation of) `usermod`.
#[context("Intercepting usermod")]
pub(crate) fn entrypoint(args: &[&str]) -> Result<()> {
    fail::fail_point!("intercept_usermod_ok", |_| Ok(()));

    // This parses the same CLI surface as the real `usermod`,
    // but in the end we only extract the user name and supplementary
    // groups.
    let matches = cli_cmd().get_matches_from(args);
    let username = matches
        .get_one::<String>("username")
        .ok_or_else(|| anyhow!("Missing required argument (username)"))?;
    if !matches.contains_id("append") {
        return Err(anyhow!("usermod called without '--append' argument"));
    };
    let groups: Vec<_> = matches
        .get_many::<String>("groups")
        .ok_or_else(|| anyhow!("Missing supplementary groups"))?
        .collect();
    if groups.is_empty() {
        return Err(anyhow!("No supplementary groups specified"));
    };

    let rootdir = Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
    for secondary_group in groups {
        generate_sysusers_fragment(&rootdir, username, secondary_group)?;
    }

    Ok(())
}

/// CLI parser, matches <https://linux.die.net/man/8/usermod>.
fn cli_cmd() -> Command {
    let name = "usermod";
    Command::new(name)
        .bin_name(name)
        .about("modify a user account")
        .arg(
            Arg::new("append")
                .short('a')
                .long("append")
                .action(ArgAction::SetTrue),
        )
        .arg(
            Arg::new("groups")
                .short('G')
                .long("groups")
                .action(ArgAction::Append),
        )
        .arg(Arg::new("username").required(true))
}

/// Write a sysusers.d configuration fragment for the given user.
///
/// This returns whether a new fragment has been actually written
/// to disk.
#[context(
    "Generating sysusers.d fragment adding user '{}' to group '{}'",
    username,
    group
)]
pub(crate) fn generate_sysusers_fragment(
    rootdir: &Dir,
    username: &str,
    group: &str,
) -> Result<bool> {
    // The filename of the configuration fragment is in fact a public
    // API, because users may have masked it in /etc. Do not change this.
    let filename = format!("40-rpmostree-pkg-usermod-{username}-{group}.conf");

    let conf_dir = common::open_create_sysusers_dir(rootdir)?;
    if conf_dir.try_exists(&filename)? {
        return Ok(false);
    }

    conf_dir
        .atomic_replace_with(&filename, |fragment| -> Result<()> {
            let perms = Permissions::from_mode(0o644);
            fragment.get_mut().as_file_mut().set_permissions(perms)?;

            fragment.write_all(b"# Generated by rpm-ostree\n")?;
            let entry = format!("m {username} {group}\n");
            fragment.write_all(entry.as_bytes())?;

            Ok(())
        })
        .with_context(|| format!("Writing /{SYSUSERS_DIR}/{filename}"))?;

    Ok(true)
}

#[cfg(test)]
mod test {
    use super::*;
    use cap_std_ext::cap_tempfile;
    use std::io::Read;

    #[test]
    fn test_clap_cmd() {
        cli_cmd().debug_assert();

        let cmd = cli_cmd();
        let extra_group = ["/usr/sbin/usermod", "-a", "-G", "tss", "clevis"];
        let matches = cmd.try_get_matches_from(extra_group).unwrap();
        assert_eq!(
            matches
                .get_many::<String>("groups")
                .unwrap()
                .into_iter()
                .collect::<Vec<_>>(),
            vec!["tss"]
        );
        assert_eq!(matches.get_one::<String>("username").unwrap(), "clevis");

        let err_cases = [
            vec!["/usr/sbin/usermod"],
            vec!["/usr/sbin/usermod", "-a", "-G", "tss"],
        ];
        for input in err_cases {
            let cmd = cli_cmd();
            cmd.try_get_matches_from(input).unwrap_err();
        }
    }

    #[test]
    fn test_fragment_generation() {
        let tmpdir = cap_tempfile::tempdir(cap_tempfile::ambient_authority()).unwrap();

        let testcases = [
            ("testuser", "bar", true),
            ("testuser", "bar", false),
            ("testuser", "other", true),
        ];
        for entry in testcases {
            let generated = generate_sysusers_fragment(&tmpdir, entry.0, entry.1).unwrap();
            assert_eq!(generated, entry.2, "{:?}", entry);

            let path = format!(
                "usr/lib/sysusers.d/40-rpmostree-pkg-usermod-{}-{}.conf",
                entry.0, entry.1
            );
            assert!(tmpdir.is_file(&path));

            let mut fragment = tmpdir.open(&path).unwrap();
            let mut content = String::new();
            fragment.read_to_string(&mut content).unwrap();
            let expected = format!("# Generated by rpm-ostree\nm {} {}\n", entry.0, entry.1);
            assert_eq!(content, expected)
        }
    }
}
