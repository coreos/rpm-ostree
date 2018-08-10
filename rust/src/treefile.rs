/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* Copied and adapted from:
 * https://github.com/cgwalters/coreos-assembler
 * */

use openat;
use serde_json;
use serde_yaml;
use std::io::prelude::*;
use std::path::{Path, PathBuf};
use std::{fs, io};
use tempfile;

const ARCH_X86_64: &'static str = "x86_64";

pub struct Treefile {
    pub workdir: openat::Dir,
    pub parsed: TreeComposeConfig,
    pub rojig_spec: Option<PathBuf>,
}

/// Parse a YAML treefile definition using architecture `arch`.
fn treefile_parse_yaml<R: io::Read>(input: R, arch: Option<&str>) -> io::Result<TreeComposeConfig> {
    let mut treefile: TreeComposeConfig = match serde_yaml::from_reader(input) {
        Ok(t) => t,
        Err(e) => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("serde: {}", e),
            ))
        }
    };

    // Special handling for packages, since we allow whitespace within items.
    // We also canonicalize bootstrap_packages to packages here so it's
    // easier to append the arch packages after.
    let mut pkgs: Vec<String> = vec![];
    {
        if let Some(base_pkgs) = treefile.packages.take() {
            pkgs.extend_from_slice(&whitespace_split_packages(&base_pkgs));
        }
        if let Some(bootstrap_pkgs) = treefile.bootstrap_packages.take() {
            pkgs.extend_from_slice(&whitespace_split_packages(&bootstrap_pkgs));
        }
    }

    let arch_pkgs = match arch {
        Some("aarch64") => treefile.packages_aarch64.take(),
        Some("armhfp") => treefile.packages_armhfp.take(),
        Some("ppc64") => treefile.packages_ppc64.take(),
        Some("ppc64le") => treefile.packages_ppc64le.take(),
        Some("s390x") => treefile.packages_s390x.take(),
        Some(ARCH_X86_64) => treefile.packages_x86_64.take(),
        None => None,
        Some(x) => panic!("Invalid architecture: {}", x),
    };
    if let Some(arch_pkgs) = arch_pkgs {
        pkgs.extend_from_slice(&whitespace_split_packages(&arch_pkgs));
    }

    treefile.packages = Some(pkgs);
    Ok(treefile)
}

impl Treefile {
    pub fn new_boxed(
        filename: &Path,
        arch: Option<&str>,
        workdir: openat::Dir,
    ) -> io::Result<Box<Treefile>> {
        let f = io::BufReader::new(fs::File::open(filename)?);
        let parsed = treefile_parse_yaml(f, arch)?;
        let rojig_spec = if let &Some(ref rojig) = &parsed.rojig {
            Some(Treefile::write_rojig_spec(&workdir, rojig)?)
        } else {
            None
        };
        Ok(Box::new(Treefile {
            parsed: parsed,
            workdir: workdir,
            rojig_spec: rojig_spec,
        }))
    }

    pub fn serialize_json_fd(&self) -> io::Result<fs::File> {
        let mut tmpf = tempfile::tempfile()?;
        {
            let output = io::BufWriter::new(&tmpf);
            serde_json::to_writer_pretty(output, &self.parsed)?;
        }
        tmpf.seek(io::SeekFrom::Start(0))?;
        Ok(tmpf)
    }

    fn write_rojig_spec<'a, 'b>(workdir: &'a openat::Dir, r: &'b Rojig) -> io::Result<PathBuf> {
        let description = r.description
            .as_ref()
            .and_then(|v| if v.len() > 0 { Some(v.as_str()) } else { None })
            .unwrap_or(r.summary.as_str());
        let name: PathBuf = format!("{}.spec", r.name).into();
        {
            let mut f = workdir.write_file(&name, 0644)?;
            write!(
                f,
                r###"
# The canonical version of this is maintained by rpm-ostree.
# Suppress most build root processing we are just carrying
# binary data.
%global __os_install_post /usr/lib/rpm/brp-compress %{{nil}}
Name: {rpmostree_rojig_name}
Version:	%{{ostree_version}}
Release:	1%{{?dist}}
Summary:	{rpmostree_rojig_summary}
License:	{rpmostree_rojig_license}
#@@@rpmostree_rojig_meta@@@

%description
{rpmostree_rojig_description}

%prep

%build

%install
mkdir -p %{{buildroot}}%{{_prefix}}/lib/ostree-jigdo/%{{name}}
for x in *; do mv ${{x}} %{{buildroot}}%{{_prefix}}/lib/ostree-jigdo/%{{name}}; done

%files
%{{_prefix}}/lib/ostree-jigdo/%{{name}}
"###,
                rpmostree_rojig_name = r.name,
                rpmostree_rojig_summary = r.summary,
                rpmostree_rojig_license = r.license,
                rpmostree_rojig_description = description,
            )?;
        }
        Ok(name)
    }
}

fn whitespace_split_packages(pkgs: &[String]) -> Vec<String> {
    pkgs.iter()
        .flat_map(|pkg| pkg.split_whitespace().map(String::from))
        .collect()
}

#[derive(Serialize, Deserialize, Debug)]
pub enum BootLocation {
    #[serde(rename = "both")]
    Both,
    #[serde(rename = "legacy")]
    Legacy,
    #[serde(rename = "new")]
    New,
}

impl Default for BootLocation {
    fn default() -> Self {
        BootLocation::Both
    }
}

#[derive(Serialize, Deserialize, Debug)]
pub enum CheckPasswdType {
    #[serde(rename = "none")]
    None,
    #[serde(rename = "previous")]
    Previous,
    #[serde(rename = "file")]
    File,
    #[serde(rename = "data")]
    Data,
}

#[derive(Serialize, Deserialize, Debug)]
pub struct CheckPasswd {
    #[serde(rename = "type")]
    variant: CheckPasswdType,
    filename: Option<String>,
    // Skip this for now, a separate file is easier
    // and anyways we want to switch to sysusers
    // entries: Option<Map<>String>,
}

#[derive(Serialize, Deserialize, Debug)]
pub struct Rojig {
    pub name: String,
    pub summary: String,
    pub license: String,
    pub description: Option<String>,
}

// https://github.com/serde-rs/serde/issues/368
fn serde_true() -> bool {
    true
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(deny_unknown_fields)]
pub struct TreeComposeConfig {
    // Compose controls
    #[serde(rename = "ref")]
    pub treeref: String,
    // Optional rojig data
    #[serde(skip_serializing_if = "Option::is_none")]
    pub rojig: Option<Rojig>,
    #[serde(skip_serializing_if = "Option::is_none")]
    repos: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub selinux: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gpg_key: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub include: Option<String>,

    // Core content
    #[serde(skip_serializing_if = "Option::is_none")]
    pub packages: Option<Vec<String>>,
    // Arch-specific packages; TODO replace this with
    // custom deserialization or so and avoid having
    // having an architecture list here.
    #[serde(rename = "packages-aarch64")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub packages_aarch64: Option<Vec<String>>,
    #[serde(rename = "packages-armhfp")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub packages_armhfp: Option<Vec<String>>,
    #[serde(rename = "packages-ppc64")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub packages_ppc64: Option<Vec<String>>,
    #[serde(rename = "packages-ppc64le")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub packages_ppc64le: Option<Vec<String>>,
    #[serde(rename = "packages-s390x")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub packages_s390x: Option<Vec<String>>,
    #[serde(rename = "packages-x86_64")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub packages_x86_64: Option<Vec<String>>,
    // Deprecated option
    #[serde(skip_serializing_if = "Option::is_none")]
    pub bootstrap_packages: Option<Vec<String>>,

    // Content installation opts
    #[serde(skip_serializing_if = "Option::is_none")]
    pub documentation: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "install-langs")]
    pub install_langs: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "initramfs-args")]
    pub initramfs_args: Option<Vec<String>>,

    // Tree layout options
    #[serde(default)]
    pub boot_location: BootLocation,
    #[serde(default)]
    #[serde(rename = "tmp-is-dir")]
    pub tmp_is_dir: bool,

    // systemd
    #[serde(skip_serializing_if = "Option::is_none")]
    pub units: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub default_target: Option<String>,
    #[serde(default = "serde_true")]
    #[serde(rename = "machineid-compat")]
    pub machineid_compat: bool,

    // versioning
    #[serde(skip_serializing_if = "Option::is_none")]
    pub releasever: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub automatic_version_prefix: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "mutate-os-release")]
    pub mutate_os_release: Option<String>,

    // passwd-related bits
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "etc-group-members")]
    pub etc_group_members: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "preserve-passwd")]
    pub preserve_passwd: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "check-passwd")]
    pub check_passwd: Option<CheckPasswd>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "check-groups")]
    pub check_groups: Option<CheckPasswd>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ignore-removed-users")]
    pub ignore_removed_users: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ignore-removed-groups")]
    pub ignore_removed_groups: Option<Vec<String>>,

    // Content manipulation
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "postprocess-script")]
    pub postprocess_script: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "add-files")]
    pub add_files: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "remove-files")]
    pub remove_files: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "remove-from-packages")]
    pub remove_from_packages: Option<Vec<Vec<String>>>,
}

#[cfg(test)]
mod tests {
    use super::*;

    static VALID_PRELUDE: &str = r###"
ref: "exampleos/x86_64/blah"
packages:
 - foo bar
 - baz
packages-x86_64:
 - grub2 grub2-tools
packages-s390x:
 - zipl
"###;

    #[test]
    fn basic_valid() {
        let input = io::BufReader::new(VALID_PRELUDE.as_bytes());
        let treefile = treefile_parse_yaml(input, Some(ARCH_X86_64)).unwrap();
        assert!(treefile.treeref == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 5);
    }

    #[test]
    fn basic_valid_noarch() {
        let input = io::BufReader::new(VALID_PRELUDE.as_bytes());
        let treefile = treefile_parse_yaml(input, None).unwrap();
        assert!(treefile.treeref == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 3);
    }

    fn test_invalid(data: &'static str) {
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(data);
        let buf = buf.as_bytes();
        let input = io::BufReader::new(buf);
        match treefile_parse_yaml(input, Some(ARCH_X86_64)) {
            Err(ref e) if e.kind() == io::ErrorKind::InvalidInput => {}
            Err(ref e) => panic!("Expected invalid treefile, not {}", e.to_string()),
            _ => panic!("Expected invalid treefile"),
        }
    }

    #[test]
    fn test_invalid_install_langs() {
        test_invalid(
            r###"install_langs:
  - "klingon"
  - "esperanto"
"###,
        );
    }

    #[test]
    fn test_invalid_arch() {
        test_invalid(
            r###"packages-hal9000:
  - podbaydoor glowingredeye
"###,
        );
    }

    struct TreefileTest {
        tf: Box<Treefile>,
        #[allow(dead_code)]
        workdir: tempfile::TempDir,
    }

    impl TreefileTest {
        fn new<'a, 'b>(contents: &'a str, arch: Option<&'b str>) -> io::Result<TreefileTest> {
            let workdir = tempfile::tempdir()?;
            let tf_path = workdir.path().join("treefile.yaml");
            {
                let mut tf_stream = io::BufWriter::new(fs::File::create(&tf_path)?);
                tf_stream.write_all(contents.as_bytes())?;
            }
            let tf =
                Treefile::new_boxed(tf_path.as_path(), arch, openat::Dir::open(workdir.path())?)?;
            Ok(TreefileTest { tf, workdir })
        }
    }

    #[test]
    fn test_treefile_new() {
        let t = TreefileTest::new(VALID_PRELUDE, None).unwrap();
        let tf = &t.tf;
        assert!(tf.parsed.rojig.is_none());
        assert!(tf.rojig_spec.is_none());
    }

    #[test]
    fn test_treefile_new_rojig() {
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(
            r###"
rojig:
  name: "exampleos"
  license: "MIT"
  summary: "ExampleOS rojig base image"
"###,
        );
        let t = TreefileTest::new(buf.as_str(), None).unwrap();
        let tf = &t.tf;
        let rojig = tf.parsed.rojig.as_ref().unwrap();
        assert!(rojig.name == "exampleos");
        let rojig_spec = tf.rojig_spec.as_ref().unwrap();
        assert!(rojig_spec.file_name().unwrap() == "exampleos.spec");
    }

}
