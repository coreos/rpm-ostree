//! CLI handler for intercepted `useradd`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use super::common::{self, SYSUSERS_DIR};
use anyhow::{anyhow, Context, Result};
use cap_std::fs::{Dir, Permissions};
use cap_std_ext::prelude::CapStdExtDirExt;
use clap::{Arg, Command};
use fn_error_context::context;
use std::io::Write;
use std::os::unix::prelude::PermissionsExt;

/// Entrypoint for (the rpm-ostree implementation of) `useradd`.
#[context("Intercepting useradd")]
pub(crate) fn entrypoint(args: &[&str]) -> Result<()> {
    fail::fail_point!("intercept_useradd_ok", |_| Ok(()));

    // Extract the package name from rpm-ostree/bwrap environment.
    // This also works as a sanity check to ensure we are running in
    // the context of a scriptlet.
    static SCRIPT_PKG_VAR: &str = "RPMOSTREE_SCRIPT_PKG_NAME";
    let pkgname = std::env::var(SCRIPT_PKG_VAR)
        .with_context(|| format!("Failed to access {SCRIPT_PKG_VAR} environment variable"))?;

    // This parses the same CLI surface as the real `useradd`,
    // but in the end we only extract the username and UID/GID
    // (if present).
    let matches = cli_cmd().get_matches_from(args);
    let username = matches
        .value_of("username")
        .ok_or_else(|| anyhow!("missing required username argument"))?;
    let uid = matches
        .value_of("uid")
        .map(|s| s.parse::<u32>())
        .transpose()?;
    let group = matches.value_of("group");
    let gecos = matches.value_of("comment");
    let homedir = matches.value_of("homedir");
    let shell = matches.value_of("shell");

    if !matches.is_present("system") {
        crate::client::warn_future_incompatibility(
            format!("Trying to create non-system user '{username}'; this will become an error in the future.")
        );
    }

    let rootdir = Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
    generate_sysusers_fragment(
        &rootdir,
        &pkgname,
        username,
        (uid, group),
        gecos,
        homedir,
        shell,
    )?;

    Ok(())
}

/// CLI parser, matches <https://linux.die.net/man/8/useradd>.
fn cli_cmd() -> Command<'static> {
    let name = "useradd";
    Command::new(name)
        .bin_name(name)
        .about("create a new user")
        .arg(
            Arg::new("comment")
                .short('c')
                .long("comment")
                .takes_value(true),
        )
        .arg(
            Arg::new("homedir")
                .short('d')
                .long("home-dir")
                .takes_value(true),
        )
        .arg(Arg::new("group").short('g').long("gid").takes_value(true))
        .arg(Arg::new("system").short('r').long("system"))
        .arg(Arg::new("shell").short('s').long("shell").takes_value(true))
        .arg(Arg::new("uid").short('u').long("uid").takes_value(true))
        .arg(Arg::new("username").required(true))
}

/// Write a sysusers.d configuration fragment for the given user.
///
/// This returns whether a new fragment has been actually written
/// to disk.
#[context(
    "Generating sysusers.d fragment for package '{}', adding user '{}'",
    pkgname,
    username
)]
fn generate_sysusers_fragment(
    rootdir: &Dir,
    pkgname: &str,
    username: &str,
    id: (Option<u32>, Option<&str>),
    gecos: Option<&str>,
    homedir: Option<&str>,
    shell: Option<&str>,
) -> Result<bool> {
    // The filename of the configuration fragment is in fact a public
    // API, because users may have masked it in /etc. Do not change this.
    let filename = format!("35-pkg-{pkgname}-user-{username}.conf");

    let conf_dir = common::open_create_sysusers_dir(rootdir)?;
    if conf_dir.try_exists(&filename)? {
        return Ok(false);
    }

    let id = match id {
        (Some(uid), Some(group)) => format!("{uid}:{group}"),
        (Some(id), None) => format!("{id}"),
        (None, Some(group)) => format!("-:{group}"),
        (None, None) => "-".to_string(),
    };

    let gecos = gecos
        .map(|g| format!("\"{g}\""))
        .unwrap_or_else(|| "-".to_string());
    let homedir = homedir
        .map(|h| {
            match h {
                // Detect and match systemd default:
                // https://github.com/systemd/systemd/blob/v251/src/sysusers/sysusers.c#L471-L472
                "" | "/" => "-",
                x => x,
            }
            .to_string()
        })
        .unwrap_or_else(|| "-".to_string());
    let shell = shell
        .map(|s| {
            match s {
                // Detect and match systemd default:
                // https://github.com/systemd/systemd/blob/v251/meson.build#L633
                "" | "/sbin/nologin" | "/usr/sbin/nologin" => "-",
                x => x,
            }
            .to_string()
        })
        .unwrap_or_else(|| "-".to_string());

    conf_dir
        .atomic_replace_with(&filename, |fragment| -> Result<()> {
            let perms = Permissions::from_mode(0o644);
            fragment.get_mut().as_file_mut().set_permissions(perms)?;

            fragment.write_all(b"# Generated by rpm-ostree\n")?;
            let entry = format!("u {username} {id} {gecos} {homedir} {shell}\n");
            fragment.write_all(entry.as_bytes())?;

            Ok(())
        })
        .with_context(|| format!("Writing /{SYSUSERS_DIR}/{filename}"))?;

    Ok(true)
}

#[cfg(test)]
mod test {
    use super::*;
    use std::io::Read;

    #[test]
    fn test_clap_cmd() {
        cli_cmd().debug_assert();

        let cmd = cli_cmd();
        let static_uid = [
            "/usr/sbin/useradd",
            "-d",
            "testhome",
            "-u",
            "7",
            "-g",
            "root",
            "halt",
        ];
        let matches = cmd.try_get_matches_from(static_uid).unwrap();
        assert_eq!(matches.value_of("homedir"), Some("testhome"));
        assert_eq!(matches.value_of("uid"), Some("7"));
        assert_eq!(matches.value_of("group"), Some("root"));
        assert_eq!(matches.value_of("username"), Some("halt"));

        let cmd = cli_cmd();
        let dynamic_uid = ["/usr/sbin/useradd", "-r", "-s", "testshell", "clevis"];
        let matches = cmd.try_get_matches_from(dynamic_uid).unwrap();
        assert!(matches.contains_id("system"));
        assert_eq!(matches.value_of("uid"), None);
        assert_eq!(matches.value_of("username"), Some("clevis"));
        assert_eq!(matches.value_of("shell"), Some("testshell"));

        let err_cases = [vec!["/usr/sbin/useradd"]];
        for input in err_cases {
            let cmd = cli_cmd();
            cmd.try_get_matches_from(input).unwrap_err();
        }
    }

    #[test]
    fn test_fragment_generation() {
        let tmpdir = cap_tempfile::tempdir(cap_tempfile::ambient_authority()).unwrap();

        let groups = [
            ("first_user", (Some(42), None), true, "42"),
            ("first_user", (None, None), false, "42"),
            ("second_user", (None, None), true, "-"),
            (
                "third_user",
                (None, Some("some_group")),
                true,
                "-:some_group",
            ),
        ];
        for entry in groups {
            let generated = generate_sysusers_fragment(
                &tmpdir,
                "foopkg",
                entry.0,
                entry.1,
                Some("freeform description"),
                None,
                None,
            )
            .unwrap();
            assert_eq!(generated, entry.2, "{:?}", entry);

            let path = format!("usr/lib/sysusers.d/35-pkg-foopkg-user-{}.conf", entry.0);
            assert!(tmpdir.is_file(&path));

            let mut fragment = tmpdir.open(&path).unwrap();
            let mut content = String::new();
            fragment.read_to_string(&mut content).unwrap();
            let expected = format!(
                "# Generated by rpm-ostree\nu {} {} \"freeform description\" - -\n",
                entry.0, entry.3
            );
            assert_eq!(content, expected)
        }
    }
}
