use anyhow::Result;
use clap::{App, Arg};

use crate::cliwrap::cliutil;
use crate::cliwrap::RunDisposition;

fn new_rpm_app<'r>() -> App<'r, 'static> {
    let name = "cli-ostree-wrapper-rpm";
    App::new(name)
        .bin_name(name)
        .version("0.1")
        .about("Wrapper for rpm")
        .arg(Arg::with_name("verify").short("V"))
        .arg(Arg::with_name("version"))
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

fn disposition(argv: &[&str]) -> Result<RunDisposition> {
    // Today rpm has --query take precendence over --erase and --install
    // apparently, so let's just accept anything with --query as there
    // are a lot of sub-options for that.
    if has_query(argv) {
        return Ok(RunDisposition::Ok);
    }

    let mut app = new_rpm_app();
    let matches = match app.get_matches_from_safe_borrow(std::iter::once(&"rpm").chain(argv.iter()))
    {
        Ok(v) => v,
        Err(e) if e.kind == clap::ErrorKind::VersionDisplayed => return Ok(RunDisposition::Ok),
        _ => return Ok(RunDisposition::Warn),
    };

    if matches.is_present("verify") {
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
pub(crate) fn main(argv: &[&str]) -> Result<()> {
    if cliutil::is_unlocked()? {
        // For now if we're unlocked, just directly exec rpm. In the future we
        // may choose to take over installing a package live.
        cliutil::exec_real_binary("rpm", argv)
    } else {
        match disposition(argv)? {
            RunDisposition::Ok => cliutil::run_unprivileged(false, "rpm", argv),
            RunDisposition::Warn => cliutil::run_unprivileged(true, "rpm", argv),
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
        assert_eq!(disposition(&["--version"])?, RunDisposition::Ok);
        Ok(())
    }

    #[test]
    fn test_query_all() -> Result<()> {
        assert_eq!(disposition(&["-qa"])?, RunDisposition::Ok);
        Ok(())
    }

    #[test]
    fn test_query_file() -> Result<()> {
        assert_eq!(
            disposition(&["--query", "-f", "/usr/bin/bash"])?,
            RunDisposition::Ok
        );
        Ok(())
    }

    #[test]
    fn test_query_requires() -> Result<()> {
        assert_eq!(
            disposition(&["--requires", "-q", "blah"])?,
            RunDisposition::Ok
        );
        Ok(())
    }

    #[test]
    fn test_query_erase() -> Result<()> {
        // Note --query overrides --erase today
        assert_eq!(disposition(&["-qea", "bash"])?, RunDisposition::Ok);
        Ok(())
    }

    #[test]
    fn test_erase() -> Result<()> {
        assert_eq!(disposition(&["--erase", "bash"])?, RunDisposition::Warn);
        Ok(())
    }

    #[test]
    fn test_shorterase() -> Result<()> {
        assert_eq!(disposition(&["-e", "bash"])?, RunDisposition::Warn);
        Ok(())
    }
}
