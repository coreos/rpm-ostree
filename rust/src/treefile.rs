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

use c_utf8::CUtf8Buf;
use openat;
use serde_json;
use serde_yaml;
use std::collections::HashMap;
use std::io::prelude::*;
use std::path::Path;
use std::{collections, fs, io};
use utils;
use failure::Fallible;
use failure::ResultExt;

const ARCH_X86_64: &'static str = "x86_64";
const INCLUDE_MAXDEPTH: u32 = 50;

/// This struct holds file descriptors for any external files/data referenced by
/// a TreeComposeConfig.
struct TreefileExternals {
    pub postprocess_script: Option<fs::File>,
    pub add_files: collections::HashMap<String, fs::File>,
    pub passwd: Option<fs::File>,
    pub group: Option<fs::File>,
}

pub struct Treefile {
    // This one isn't used today, but we may do more in the future.
    _workdir: openat::Dir,
    primary_dfd: openat::Dir,
    #[allow(dead_code)] // Not used in tests
    parsed: TreeComposeConfig,
    // This is a copy of rojig.name to avoid needing to convert to CStr when reading
    rojig_name: Option<CUtf8Buf>,
    rojig_spec: Option<CUtf8Buf>,
    serialized: CUtf8Buf,
    externals: TreefileExternals,
}

// We only use this while parsing
struct ConfigAndExternals {
    config: TreeComposeConfig,
    externals: TreefileExternals,
}

enum InputFormat {
    YAML,
    JSON,
}

/// Parse a YAML treefile definition using architecture `arch`.
/// This does not open the externals.
fn treefile_parse_stream<R: io::Read>(
    fmt: InputFormat,
    input: &mut R,
    arch: Option<&str>,
) -> Fallible<TreeComposeConfig> {
    let mut treefile: TreeComposeConfig = match fmt {
        InputFormat::YAML => {
            let tf: StrictTreeComposeConfig = serde_yaml::from_reader(input).map_err(|e| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("serde-yaml: {}", e.to_string()),
                )
            })?;
            tf.config
        }
        InputFormat::JSON => {
            let tf: PermissiveTreeComposeConfig = serde_json::from_reader(input).map_err(|e| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("serde-json: {}", e.to_string()),
                )
            })?;
            tf.config
        }
    };

    // Substitute ${basearch}
    treefile.treeref = match (arch, treefile.treeref.take()) {
        (Some(arch), Some(treeref)) => {
            let mut varsubsts = HashMap::new();
            varsubsts.insert("basearch".to_string(), arch.to_string());
            Some(
                utils::varsubst(&treeref, &varsubsts)
                    .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e.to_string()))?,
            )
        }
        (_, v) => v,
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

/// Open file and provide context containing filename on failures.
fn open_file<P: AsRef<Path>>(filename: P) -> Fallible<fs::File> {
    return Ok(fs::File::open(filename.as_ref()).with_context(
        |e| format!("Can't open file {:?}: {}", filename.as_ref().display(), e))?);
}

// If a passwd/group file is provided explicitly, load it as a fd
fn load_passwd_file<P: AsRef<Path>>(
    basedir: P,
    v: &Option<CheckPasswd>,
) -> Fallible<Option<fs::File>> {
    if let &Some(ref v) = v {
        let basedir = basedir.as_ref();
        if let Some(ref path) = v.filename {
            return Ok(Some(open_file(basedir.join(path))?));
        }
    }
    return Ok(None);
}

/// Given a treefile filename and an architecture, parse it and also
/// open its external files.
fn treefile_parse<P: AsRef<Path>>(
    filename: P,
    arch: Option<&str>,
) -> Fallible<ConfigAndExternals> {
    let filename = filename.as_ref();
    let mut f = io::BufReader::new(open_file(filename)?);
    let basename = filename
        .file_name()
        .map(|s| s.to_string_lossy())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "Expected a filename"))?;
    let fmt = if basename.ends_with(".yaml") || basename.ends_with(".yml") {
        InputFormat::YAML
    } else {
        InputFormat::JSON
    };
    let tf = treefile_parse_stream(fmt, &mut f, arch).map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("Parsing {}: {}", filename.to_string_lossy(), e.to_string()),
        )
    })?;
    let postprocess_script = if let Some(ref postprocess) = tf.postprocess_script.as_ref() {
        Some(open_file(filename.with_file_name(postprocess))?)
    } else {
        None
    };
    let mut add_files: collections::HashMap<String, fs::File> = collections::HashMap::new();
    if let Some(ref add_file_names) = tf.add_files.as_ref() {
        for (name, _) in add_file_names.iter() {
            add_files.insert(name.clone(), open_file(filename.with_file_name(name))?);
        }
    }
    let parent = filename.parent().unwrap();
    let passwd = load_passwd_file(&parent, &tf.check_passwd)?;
    let group = load_passwd_file(&parent, &tf.check_groups)?;
    Ok(ConfigAndExternals {
        config: tf,
        externals: TreefileExternals {
            postprocess_script,
            add_files,
            passwd,
            group,
        },
    })
}

/// Merge a "basic" or non-array field. The semantics originally defined for
/// these are that first-one wins.
fn merge_basic_field<T>(dest: &mut Option<T>, src: &mut Option<T>) {
    if dest.is_some() {
        return;
    }
    *dest = src.take()
}

/// Merge a vector field by appending. This semantic was originally designed for
/// the `packages` key.
fn merge_vec_field<T>(dest: &mut Option<Vec<T>>, src: &mut Option<Vec<T>>) {
    if let Some(mut srcv) = src.take() {
        if let Some(ref mut destv) = dest {
            srcv.append(destv);
        }
        *dest = Some(srcv);
    }
}

/// Given two configs, merge them.
fn treefile_merge(dest: &mut TreeComposeConfig, src: &mut TreeComposeConfig) {
    macro_rules! merge_basics {
        ( $($field:ident),* ) => {{
            $( merge_basic_field(&mut dest.$field, &mut src.$field); )*
        }};
    };
    macro_rules! merge_vecs {
        ( $($field:ident),* ) => {{
            $( merge_vec_field(&mut dest.$field, &mut src.$field); )*
        }};
    };

    merge_basics!(
        treeref,
        rojig,
        selinux,
        gpg_key,
        include,
        recommends,
        documentation,
        boot_location,
        tmp_is_dir,
        default_target,
        machineid_compat,
        releasever,
        automatic_version_prefix,
        mutate_os_release,
        etc_group_members,
        preserve_passwd,
        check_passwd,
        check_groups,
        ignore_removed_users,
        ignore_removed_groups,
        postprocess_script
    );
    merge_vecs!(
        repos,
        packages,
        install_langs,
        initramfs_args,
        units,
        postprocess,
        add_files,
        remove_files,
        remove_from_packages
    );
}

/// Merge the treefile externals. There are currently only two keys that
/// reference external files.
fn treefile_merge_externals(dest: &mut TreefileExternals, src: &mut TreefileExternals) {
    // This one, being a basic-valued field, has first-wins semantics.
    if dest.postprocess_script.is_none() {
        dest.postprocess_script = src.postprocess_script.take();
    }

    // add-files is an array and hence has append semantics.
    for (k, v) in src.add_files.drain() {
        dest.add_files.insert(k, v);
    }

    // passwd/group are basic values
    if dest.passwd.is_none() {
        dest.passwd = src.passwd.take();
    }
    if dest.group.is_none() {
        dest.group = src.group.take();
    }
}

/// Recursively parse a treefile, merging along the way.
fn treefile_parse_recurse<P: AsRef<Path>>(
    filename: P,
    arch: Option<&str>,
    depth: u32,
) -> Fallible<ConfigAndExternals> {
    let filename = filename.as_ref();
    let mut parsed = treefile_parse(filename, arch)?;
    let include_path = parsed.config.include.take();
    if let &Some(ref include_path) = &include_path {
        if depth == INCLUDE_MAXDEPTH {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Reached maximum include depth {}", INCLUDE_MAXDEPTH),
            ).into());
        }
        let parent = filename.parent().unwrap();
        let include_path = parent.join(include_path);
        let mut included = treefile_parse_recurse(include_path, arch, depth + 1)?;
        treefile_merge(&mut parsed.config, &mut included.config);
        treefile_merge_externals(&mut parsed.externals, &mut included.externals);
    }
    Ok(parsed)
}

// Similar to the importer check but just checks for prefixes since
// they're files, and also allows /etc since it's before conversion
fn add_files_path_is_valid(path: &str) -> bool {
    let path = path.trim_left_matches("/");
    (path.starts_with("usr/") && !path.starts_with("usr/local/"))
        || path.starts_with("etc/")
        || path.starts_with("bin/")
        || path.starts_with("sbin/")
        || path.starts_with("lib/")
        || path.starts_with("lib64/")
}

impl Treefile {
    /// The main treefile creation entrypoint.
    fn new_boxed(
        filename: &Path,
        arch: Option<&str>,
        workdir: openat::Dir,
    ) -> Fallible<Box<Treefile>> {
        let parsed = treefile_parse_recurse(filename, arch, 0)?;
        Treefile::validate_config(&parsed.config)?;
        let dfd = openat::Dir::open(filename.parent().unwrap())?;
        let (rojig_name, rojig_spec) = if let Some(rojig) = parsed.config.rojig.as_ref() {
            (
                Some(CUtf8Buf::from_string(rojig.name.clone())),
                Some(Treefile::write_rojig_spec(&workdir, rojig)?),
            )
        } else {
            (None, None)
        };
        let serialized = Treefile::serialize_json_string(&parsed.config)?;
        Ok(Box::new(Treefile {
            primary_dfd: dfd,
            parsed: parsed.config,
            _workdir: workdir,
            rojig_name: rojig_name,
            rojig_spec: rojig_spec,
            serialized: serialized,
            externals: parsed.externals,
        }))
    }

    /// Do some upfront semantic checks we can do beyond just the type safety serde provides.
    fn validate_config(config: &TreeComposeConfig) -> Fallible<()> {
        // check add-files
        if let Some(files) = &config.add_files {
            for (_, dest) in files.iter() {
                if !add_files_path_is_valid(&dest) {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        format!("Unsupported path in add-files: {}", dest),
                    ).into());
                }
            }
        }
        Ok(())
    }

    fn serialize_json_string(config: &TreeComposeConfig) -> Fallible<CUtf8Buf> {
        let mut output = vec![];
        serde_json::to_writer_pretty(&mut output, config)?;
        Ok(CUtf8Buf::from_string(
            String::from_utf8(output).expect("utf-8 json"),
        ))
    }

    /// Generate a rojig spec file.
    fn write_rojig_spec<'a, 'b>(workdir: &'a openat::Dir, r: &'b Rojig) -> Fallible<CUtf8Buf> {
        let description = r
            .description
            .as_ref()
            .and_then(|v| if v.len() > 0 { Some(v.as_str()) } else { None })
            .unwrap_or(r.summary.as_str());
        let name: String = format!("{}.spec", r.name);
        {
            let mut f = workdir.write_file(name.as_str(), 0o644)?;
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
        Ok(CUtf8Buf::from_string(name))
    }
}

/// For increased readability in YAML/JSON, we support whitespace in individual
/// array elements.
fn whitespace_split_packages(pkgs: &[String]) -> Vec<String> {
    pkgs.iter()
        .flat_map(|pkg| pkg.split_whitespace().map(String::from))
        .collect()
}

#[derive(Serialize, Deserialize, Debug)]
enum BootLocation {
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
enum CheckPasswdType {
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
struct CheckPasswd {
    #[serde(rename = "type")]
    variant: CheckPasswdType,
    filename: Option<String>,
    // Skip this for now, a separate file is easier
    // and anyways we want to switch to sysusers
    // entries: Option<Map<>String>,
}

#[derive(Serialize, Deserialize, Debug)]
struct Rojig {
    name: String,
    summary: String,
    license: String,
    description: Option<String>,
}

// Because of how we handle includes, *everything* here has to be
// Option<T>.  The defaults live in the code (e.g. machineid-compat defaults
// to `true`).
#[derive(Serialize, Deserialize, Debug)]
struct TreeComposeConfig {
    // Compose controls
    #[serde(rename = "ref")]
    #[serde(skip_serializing_if = "Option::is_none")]
    treeref: Option<String>,
    // Optional rojig data
    #[serde(skip_serializing_if = "Option::is_none")]
    rojig: Option<Rojig>,
    #[serde(skip_serializing_if = "Option::is_none")]
    repos: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    selinux: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    gpg_key: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    include: Option<String>,

    // Core content
    #[serde(skip_serializing_if = "Option::is_none")]
    packages: Option<Vec<String>>,
    // Arch-specific packages; TODO replace this with
    // custom deserialization or so and avoid having
    // having an architecture list here.
    #[serde(rename = "packages-aarch64")]
    #[serde(skip_serializing_if = "Option::is_none")]
    packages_aarch64: Option<Vec<String>>,
    #[serde(rename = "packages-armhfp")]
    #[serde(skip_serializing_if = "Option::is_none")]
    packages_armhfp: Option<Vec<String>>,
    #[serde(rename = "packages-ppc64")]
    #[serde(skip_serializing_if = "Option::is_none")]
    packages_ppc64: Option<Vec<String>>,
    #[serde(rename = "packages-ppc64le")]
    #[serde(skip_serializing_if = "Option::is_none")]
    packages_ppc64le: Option<Vec<String>>,
    #[serde(rename = "packages-s390x")]
    #[serde(skip_serializing_if = "Option::is_none")]
    packages_s390x: Option<Vec<String>>,
    #[serde(rename = "packages-x86_64")]
    #[serde(skip_serializing_if = "Option::is_none")]
    packages_x86_64: Option<Vec<String>>,
    // Deprecated option
    #[serde(skip_serializing_if = "Option::is_none")]
    bootstrap_packages: Option<Vec<String>>,

    // Content installation opts
    #[serde(skip_serializing_if = "Option::is_none")]
    container: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    recommends: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    documentation: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "install-langs")]
    install_langs: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "initramfs-args")]
    initramfs_args: Option<Vec<String>>,

    // Tree layout options
    #[serde(skip_serializing_if = "Option::is_none")]
    boot_location: Option<BootLocation>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "tmp-is-dir")]
    tmp_is_dir: Option<bool>,

    // systemd
    #[serde(skip_serializing_if = "Option::is_none")]
    units: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    default_target: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "machineid-compat")]
    // Defaults to `true`
    machineid_compat: Option<bool>,

    // versioning
    #[serde(skip_serializing_if = "Option::is_none")]
    releasever: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    automatic_version_prefix: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "mutate-os-release")]
    mutate_os_release: Option<String>,

    // passwd-related bits
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "etc-group-members")]
    etc_group_members: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "preserve-passwd")]
    preserve_passwd: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "check-passwd")]
    check_passwd: Option<CheckPasswd>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "check-groups")]
    check_groups: Option<CheckPasswd>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ignore-removed-users")]
    ignore_removed_users: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ignore-removed-groups")]
    ignore_removed_groups: Option<Vec<String>>,

    // Content manipulation
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "postprocess-script")]
    // This one references an external filename
    postprocess_script: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    // This one is inline, and supports multiple (hence is useful for inheritance)
    postprocess: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "add-files")]
    add_files: Option<Vec<(String, String)>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "remove-files")]
    remove_files: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "remove-from-packages")]
    remove_from_packages: Option<Vec<Vec<String>>>,
}

#[derive(Serialize, Deserialize, Debug)]
struct PermissiveTreeComposeConfig {
    #[serde(flatten)]
    config: TreeComposeConfig,
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(deny_unknown_fields)]
struct StrictTreeComposeConfig {
    #[serde(flatten)]
    config: TreeComposeConfig,
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile;

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

    // This one has "comments" (hence unknown keys)
    static VALID_PRELUDE_JS: &str = r###"
{
 "ref": "exampleos/${basearch}/blah",
 "comment-packages": "We want baz to enable frobnication",
 "packages": ["foo", "bar", "baz"],
 "packages-x86_64": ["grub2", "grub2-tools"],
 "comment-packages-s390x": "Note that s390x uses its own bootloader",
 "packages-s390x": ["zipl"]
}
"###;

    #[test]
    fn basic_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE.as_bytes());
        let treefile =
            treefile_parse_stream(InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        assert!(treefile.treeref.unwrap() == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 5);
    }

    #[test]
    fn basic_valid_add_remove_files() {
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(
            r###"
add-files:
  - - foo
    - /usr/bin/foo
  - - baz
    - /usr/bin/blah
remove-files:
 - foo
 - bar
"###,
        );
        let buf = buf.as_bytes();
        let mut input = io::BufReader::new(buf);
        let treefile =
            treefile_parse_stream(InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        assert!(treefile.add_files.unwrap().len() == 2);
        assert!(treefile.remove_files.unwrap().len() == 2);
    }

    #[test]
    fn basic_js_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE_JS.as_bytes());
        let treefile =
            treefile_parse_stream(InputFormat::JSON, &mut input, Some(ARCH_X86_64)).unwrap();
        assert!(treefile.treeref.unwrap() == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 5);
    }

    #[test]
    fn basic_valid_noarch() {
        let mut input = io::BufReader::new(VALID_PRELUDE.as_bytes());
        let treefile = treefile_parse_stream(InputFormat::YAML, &mut input, None).unwrap();
        assert!(treefile.treeref.unwrap() == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 3);
    }

    fn test_invalid(data: &'static str) {
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(data);
        let buf = buf.as_bytes();
        let mut input = io::BufReader::new(buf);
        match treefile_parse_stream(InputFormat::YAML, &mut input, Some(ARCH_X86_64)) {
            Err(ref e) => {
                match e.downcast_ref::<io::Error>() {
                    Some(ref ioe) if ioe.kind() == io::ErrorKind::InvalidInput => {},
                    _ =>  panic!("Expected invalid treefile, not {}", e.to_string()),
                }
            }
            Ok(_) => panic!("Expected invalid treefile"),
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
        fn new<'a, 'b>(contents: &'a str, arch: Option<&'b str>) -> Fallible<TreefileTest> {
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
        assert!(tf.parsed.machineid_compat.is_none());
    }

    const ROJIG_YAML: &'static str = r###"
rojig:
  name: "exampleos"
  license: "MIT"
  summary: "ExampleOS rojig base image"
"###;

    #[test]
    fn test_treefile_new_rojig() {
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(ROJIG_YAML);
        let t = TreefileTest::new(buf.as_str(), None).unwrap();
        let tf = &t.tf;
        let rojig = tf.parsed.rojig.as_ref().unwrap();
        assert!(rojig.name == "exampleos");
        let rojig_spec_str = tf.rojig_spec.as_ref().unwrap().as_str();
        let rojig_spec = Path::new(rojig_spec_str);
        assert!(rojig_spec.file_name().unwrap() == "exampleos.spec");
    }

    #[test]
    fn test_treefile_merge() {
        let arch = Some(ARCH_X86_64);
        let mut base_input = io::BufReader::new(VALID_PRELUDE.as_bytes());
        let mut base = treefile_parse_stream(InputFormat::YAML, &mut base_input, arch).unwrap();
        let mut mid_input = io::BufReader::new(
            r###"
packages:
  - some layered packages
"###.as_bytes(),
        );
        let mut mid = treefile_parse_stream(InputFormat::YAML, &mut mid_input, arch).unwrap();
        let mut top_input = io::BufReader::new(ROJIG_YAML.as_bytes());
        let mut top = treefile_parse_stream(InputFormat::YAML, &mut top_input, arch).unwrap();
        treefile_merge(&mut mid, &mut base);
        treefile_merge(&mut top, &mut mid);
        let tf = &top;
        assert!(tf.packages.as_ref().unwrap().len() == 8);
        let rojig = tf.rojig.as_ref().unwrap();
        assert!(rojig.name == "exampleos");
    }

    #[test]
    fn test_open_file_nonexistent() {
        let path = "/usr/share/empty/manifest.yaml";
        match treefile_parse(path, None) {
            Err(ref e) => assert!(e.to_string().starts_with(
                    format!("Can't open file {:?}:", path).as_str())),
            Ok(_) => panic!("Expected nonexistent treefile error for {}", path),
        }
    }
}

mod ffi {
    use super::*;
    use ffiutil::*;
    use glib_sys;
    use libc;

    use std::io::Seek;
    use std::os::unix::io::{AsRawFd, RawFd};
    use std::{fs, io, ptr};

    // Some of our file descriptors may be read multiple times.
    // We try to consistently seek to the start to make that
    // convenient from the C side.  Note that this function
    // will abort if seek() fails (it really shouldn't).
    fn raw_seeked_fd(fd: &mut fs::File) -> RawFd {
        fd.seek(io::SeekFrom::Start(0)).expect("seek");
        fd.as_raw_fd()
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_new(
        filename: *const libc::c_char,
        arch: *const libc::c_char,
        workdir_dfd: libc::c_int,
        gerror: *mut *mut glib_sys::GError,
    ) -> *mut Treefile {
        // Convert arguments
        let filename = ffi_view_os_str(filename);
        let arch = ffi_view_nullable_str(arch);
        let workdir = ffi_view_openat_dir(workdir_dfd);
        // Run code, map error if any, otherwise extract raw pointer, passing
        // ownership back to C.
        ptr_glib_error(
            Treefile::new_boxed(filename.as_ref(), arch, workdir),
            gerror,
        )
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_dfd(tf: *mut Treefile) -> libc::c_int {
        ref_from_raw_ptr(tf).primary_dfd.as_raw_fd()
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_postprocess_script_fd(tf: *mut Treefile) -> libc::c_int {
        ref_from_raw_ptr(tf)
            .externals
            .postprocess_script
            .as_mut()
            .map_or(-1, raw_seeked_fd)
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_add_file_fd(
        tf: *mut Treefile,
        filename: *const libc::c_char,
    ) -> libc::c_int {
        let tf = ref_from_raw_ptr(tf);
        let filename = ffi_view_os_str(filename);
        let filename = filename.to_string_lossy().into_owned();
        raw_seeked_fd(tf.externals.add_files.get_mut(&filename).expect("add-file"))
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_passwd_fd(tf: *mut Treefile) -> libc::c_int {
        ref_from_raw_ptr(tf)
            .externals
            .passwd
            .as_mut()
            .map_or(-1, raw_seeked_fd)
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_group_fd(tf: *mut Treefile) -> libc::c_int {
        ref_from_raw_ptr(tf)
            .externals
            .group
            .as_mut()
            .map_or(-1, raw_seeked_fd)
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_json_string(tf: *mut Treefile) -> *const libc::c_char {
        ref_from_raw_ptr(tf).serialized.as_ptr()
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_rojig_spec_path(tf: *mut Treefile) -> *const libc::c_char {
        let tf = ref_from_raw_ptr(tf);
        if let &Some(ref rojig) = &tf.rojig_spec {
            rojig.as_ptr()
        } else {
            ptr::null_mut()
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_rojig_name(tf: *mut Treefile) -> *const libc::c_char {
        assert!(!tf.is_null());
        let tf = unsafe { &mut *tf };
        tf.rojig_name
            .as_ref()
            .map(|v| v.as_ptr())
            .unwrap_or(ptr::null_mut())
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_free(tf: *mut Treefile) {
        if tf.is_null() {
            return;
        }
        unsafe {
            Box::from_raw(tf);
        }
    }

}
pub use self::ffi::*;
