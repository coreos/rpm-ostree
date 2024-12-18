// SPDX-License-Identifier: Apache-2.0 OR MIT

//! This is a Rust implementation of a small subset of the kickstart
//! file format. It may grow in the future.

use std::{
    collections::HashSet,
    io::{BufReader, Read},
};

use anyhow::{Context, Result};
use cap_std::fs::{Dir, MetadataExt};
use clap::Parser;

// Cap includes just to avoid stack overflow in pathological cases
const MAX_INCLUDES: usize = 256;
const INCLUDE: &str = "%include";
const PACKAGES: &str = "%packages";
const END: &str = "%end";

#[derive(clap::Parser, Debug)]
pub(crate) struct PackageArgs {
    /// Do not include weak dependencies
    #[clap(long)]
    pub(crate) exclude_weakdeps: bool,
}

#[derive(Debug)]
pub(crate) struct Packages {
    pub(crate) args: PackageArgs,
    pub(crate) install: Vec<String>,
    pub(crate) excludes: Vec<String>,
}

#[derive(Debug)]
pub(crate) struct Kickstart {
    pub(crate) includes: Vec<String>,
    pub(crate) packages: Vec<Packages>,
}

#[derive(Debug)]
pub(crate) struct KickstartParsed {
    pub(crate) packages: Vec<Packages>,
}

fn filtermap_line(line: &str) -> Option<&str> {
    // Ignore comments
    if line.starts_with('#') {
        return None;
    }
    let line = line.trim();
    if line.is_empty() {
        return None;
    }
    Some(line)
}

impl Packages {
    fn parse<'a, 'b>(
        args: impl Iterator<Item = &'b str>,
        lines: impl Iterator<Item = &'a str>,
    ) -> Result<Self> {
        // Ensure there's an argv0
        let args = [PACKAGES].into_iter().chain(args);
        let args = PackageArgs::try_parse_from(args)?;
        let mut install = Vec::new();
        let mut excludes = Vec::new();
        for line in lines {
            let line = line.trim();
            if line == END {
                return Ok(Self {
                    args,
                    install,
                    excludes,
                });
            }
            if let Some(rest) = line.strip_prefix('-') {
                excludes.push(rest.to_owned());
            } else {
                install.push(line.to_owned());
            }
        }
        anyhow::bail!("Missing {END} for {PACKAGES}")
    }
}

impl Kickstart {
    pub(crate) fn parse(s: &str) -> Result<Self> {
        let mut includes = Vec::new();
        let mut packages = Vec::new();
        let mut lines = s.lines().filter_map(filtermap_line);
        while let Some(line) = lines.next() {
            let line =
                shlex::split(line).ok_or_else(|| anyhow::anyhow!("Invalid syntax: {line}"))?;
            let mut line = line.iter();
            let Some(verb) = line.next() else { continue };
            let mut line = line.map(|s| s.as_str());
            match verb.as_str() {
                PACKAGES => {
                    packages.push(Packages::parse(line, &mut lines)?);
                }
                INCLUDE => {
                    let include = line
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("Missing path for {INCLUDE}"))?;
                    if line.next().is_some() {
                        anyhow::bail!("Too many arguments for {INCLUDE}");
                    }
                    includes.push(include.to_owned());
                }
                o => {
                    anyhow::bail!("Unhandled verb: {o}")
                }
            }
        }
        Ok(Self { includes, packages })
    }
}

impl KickstartParsed {
    pub(crate) fn new(d: &Dir, path: &str) -> Result<Self> {
        let mut loaded = HashSet::new();
        Self::new_recurse(d, path, &mut loaded)
    }

    pub(crate) fn new_recurse(
        d: &Dir,
        path: &str,
        loaded: &mut HashSet<(u64, u64)>,
    ) -> Result<Self> {
        let mut ret = KickstartParsed {
            packages: Vec::new(),
        };
        let f = d.open(path).with_context(|| format!("Opening {path}"))?;
        let devino = f.metadata().map(|m| (m.dev(), m.ino()))?;
        if !loaded.insert(devino) {
            anyhow::bail!("Recursive include: {path}");
        }
        anyhow::ensure!(loaded.len() < MAX_INCLUDES);
        let mut f = BufReader::new(f);
        let mut s = String::new();
        f.read_to_string(&mut s)?;
        let ks = Kickstart::parse(&s)?;
        ret.packages.extend(ks.packages);
        for include in ks.includes {
            let child = KickstartParsed::new_recurse(d, &include, loaded)?;
            ret.packages.extend(child.packages);
        }
        Ok(ret)
    }
}

#[cfg(test)]
mod tests {
    use cap_std_ext::cap_tempfile::TempDir;

    use super::*;

    #[test]
    fn test_filtermap_line() {
        let nones = ["", "   ", "# foo"];
        for v in nones.iter() {
            assert_eq!(filtermap_line(v), None);
        }
        let idem = ["foo bar baz"];
        for &v in idem.iter() {
            assert_eq!(filtermap_line(v), Some(v));
        }
    }

    #[test]
    fn test_basic() {
        let ks = Kickstart::parse(indoc::indoc! { r#"
            # This is a comment
            %include foo.ks
            # Include this
            %include subdir/bar.ks
            # Blank line below

            %packages
            foo
            -bar
            baz
            %end
        "# })
        .unwrap();
        assert_eq!(ks.includes.len(), 2);
        assert_eq!(ks.includes[1].as_str(), "subdir/bar.ks");
        assert_eq!(ks.packages.len(), 1);
        let pkgs = ks.packages.first().unwrap();
        assert_eq!(pkgs.install.len(), 2);
        assert_eq!(pkgs.excludes.len(), 1);
    }

    #[test]
    fn test_load_from_dir() -> Result<()> {
        let td = TempDir::new(cap_std::ambient_authority())?;
        td.write("empty.ks", "")?;
        let ks = KickstartParsed::new(&td, "empty.ks").unwrap();
        assert_eq!(ks.packages.len(), 0);

        td.create_dir("subdir")?;
        td.write(
            "subdir/inc.ks",
            indoc::indoc! { r#"
            %packages --exclude-weakdeps
            systemd
            # Let's go modern
            -bash
            nushell
            %end
        "#},
        )?;

        td.write(
            "basic.ks",
            indoc::indoc! { r#"
            %packages
            foo
            -bar
            baz
            %end
            # Our includes
            %include empty.ks
            %include subdir/inc.ks
        "#},
        )?;

        let ks = KickstartParsed::new(&td, "basic.ks").unwrap();
        assert_eq!(ks.packages.len(), 2);
        let pkgs = &ks.packages[0];
        assert!(!pkgs.args.exclude_weakdeps);
        assert_eq!(pkgs.install[0], "foo");
        assert!(ks.packages[1].args.exclude_weakdeps);
        assert_eq!(ks.packages[1].excludes[0], "bash");

        Ok(())
    }

    #[test]
    fn test_loop() -> Result<()> {
        let td = TempDir::new(cap_std::ambient_authority())?;
        td.write("recursive1.ks", "%include recursive2.ks")?;
        td.write("recursive2.ks", "%include recursive1.ks")?;
        assert!(KickstartParsed::new(&td, "recursive1.ks").is_err());
        Ok(())
    }

    #[test]
    fn test_parse_err() {
        let errs = ["%packages\n", "%packages --foo\n%end\n"];
        for err in errs {
            assert!(Kickstart::parse(err).is_err());
        }
    }
}
