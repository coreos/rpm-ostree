// SPDX-License-Identifier: Apache-2.0 OR MIT
use anyhow::Result;
use clap::{Arg, Command};

use crate::cliwrap::cliutil;
use crate::cliwrap::RunDisposition;
use crate::ffi::SystemHostType;

fn new_rpm_app() -> Command {
    let name = "cli-ostree-wrapper-rpm";
    Command::new(name)
        .bin_name(name)
        .disable_version_flag(true)
        .version("0.1")
        .about("Wrapper for rpm")
        .arg(Arg::new("verify").short('V').long("verify"))
        .arg(
            Arg::new("version")
                .long("version")
                .action(clap::ArgAction::Version),
        )
        .arg(
            Arg::new("eval")
                .long("eval")
                .short('E')
                .action(clap::ArgAction::Set),
        )
        .arg(
            Arg::new("package")
                .help("package")
                .action(clap::ArgAction::Append),
        )
}

// clap doesn't easily allow us to parse unknown arguments right now,
// scan argv manually.
// https://github.com/clap-rs/clap/issues/873#issuecomment-436546860
fn has_query(argv: &[&str]) -> bool {
    for a in argv {
        let a = *a;
        if a == "--query" {
            return true;
        }
        if a.starts_with('-') && !a.starts_with("--") {
            for c in a.chars().skip(1) {
                if c == 'q' {
                    return true;
                }
            }
        }
    }
    false
}

fn disposition(host: SystemHostType, argv: &[&str]) -> Result<RunDisposition> {
    // For now, all rpm invocations are directly passed through
    match host {
        SystemHostType::OstreeContainer => return Ok(RunDisposition::Ok),
        SystemHostType::OstreeHost => {}
        _ => return Ok(RunDisposition::Unsupported),
    };

    // Today rpm has --query take precendence over --erase and --install
    // apparently, so let's just accept anything with --query as there
    // are a lot of sub-options for that.
    if has_query(argv) {
        return Ok(RunDisposition::Ok);
    }

    let app = new_rpm_app();
    let matches = match app.try_get_matches_from(std::iter::once(&"rpm").chain(argv.iter())) {
        Ok(v) => v,
        Err(e) if e.kind() == clap::error::ErrorKind::DisplayVersion => {
            return Ok(RunDisposition::Ok)
        }
        Err(_) => {
            return Ok(RunDisposition::Warn);
        }
    };

    if matches.contains_id("verify") {
        Ok(RunDisposition::Notice(
            "rpm --verify is not necessary for ostree-based systems.
            All binaries in /usr are underneath a read-only bind mount.
            If you wish to verify integrity, use `ostree fsck`."
                .to_string(),
        ))
    } else {
        // This currently really shoudln't happen, but in the future we might
        // clearly whitelist other arguments besides --query.
        Ok(RunDisposition::Ok)
    }
}

/// Primary entrypoint to running our wrapped `rpm` handling.
pub(crate) fn main(host: SystemHostType, argv: &[&str]) -> Result<()> {
    let is_unlocked_ostree = host == SystemHostType::OstreeHost && cliutil::is_unlocked()?;
    // For now if we're in a container or unlocked, just directly exec rpm. In the future we
    // may choose to actually redirect commands like `rpm -e foo` to `rpm-ostree uninstall foo`.
    if host == SystemHostType::OstreeContainer || is_unlocked_ostree {
        cliutil::exec_real_binary("rpm", argv)
    } else {
        match disposition(host, argv)? {
            RunDisposition::Ok => cliutil::run_unprivileged(false, "rpm", argv),
            RunDisposition::Warn => cliutil::run_unprivileged(true, "rpm", argv),
            RunDisposition::Unsupported => Err(anyhow::anyhow!(
                "This command is only supported on ostree-based systems."
            )),
            RunDisposition::Notice(ref s) => {
                println!("{}", s);
                Ok(())
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_version() -> Result<()> {
        assert_eq!(
            disposition(SystemHostType::OstreeHost, &["--version"])?,
            RunDisposition::Ok
        );
        Ok(())
    }

    #[test]
    fn test_query_all() -> Result<()> {
        assert_eq!(
            disposition(SystemHostType::OstreeHost, &["-qa"])?,
            RunDisposition::Ok
        );
        Ok(())
    }

    #[test]
    fn test_eval() -> Result<()> {
        assert_eq!(
            disposition(SystemHostType::OstreeHost, &["-E", "%{_target_cpu}"])?,
            RunDisposition::Ok
        );
        assert_eq!(
            disposition(SystemHostType::OstreeHost, &["--eval=%{_target_cpu}}"])?,
            RunDisposition::Ok
        );
        Ok(())
    }

    #[test]
    fn test_query_file() -> Result<()> {
        assert_eq!(
            disposition(
                SystemHostType::OstreeHost,
                &["--query", "-f", "/usr/bin/bash"]
            )?,
            RunDisposition::Ok
        );
        Ok(())
    }

    #[test]
    fn test_query_requires() -> Result<()> {
        assert_eq!(
            disposition(SystemHostType::OstreeHost, &["--requires", "-q", "blah"])?,
            RunDisposition::Ok
        );
        Ok(())
    }

    #[test]
    fn test_query_erase() -> Result<()> {
        // Note --query overrides --erase today
        assert_eq!(
            disposition(SystemHostType::OstreeHost, &["-qea", "bash"])?,
            RunDisposition::Ok
        );
        Ok(())
    }

    #[test]
    fn test_erase() -> Result<()> {
        assert_eq!(
            disposition(SystemHostType::OstreeHost, &["--erase", "bash"])?,
            RunDisposition::Warn
        );
        Ok(())
    }

    #[test]
    fn test_shorterase() -> Result<()> {
        assert_eq!(
            disposition(SystemHostType::OstreeHost, &["-e", "bash"])?,
            RunDisposition::Warn
        );
        Ok(())
    }

    #[test]
    fn test_verify() -> Result<()> {
        assert!(matches!(
            disposition(SystemHostType::OstreeHost, &["--verify", "bash"])?,
            RunDisposition::Notice(_)
        ));
        Ok(())
    }
}
