/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

//! Code for handling the "treefile" which is a declarative
//! config file describing how to generate an OSTree commit
//! from a set of RPMs on the server side.  Today this
//! code is not used client side; a separate OSTree "origin"
//! file is used for that.  See also
//! [this issue](https://github.com/coreos/rpm-ostree/issues/2326).

/*
 * HACKING: In order to add a entry to the treefile, be sure to:
 * - Update docs/treefile.md
 * - Add it to the struct
 * - Add a merge entry to `treefile_merge()`
 * - Add a test case in tests/compose
 */

use crate::cxxrsutil::*;
use anyhow::{anyhow, bail, Context, Result};
use c_utf8::CUtf8Buf;
use nix::unistd::{Gid, Uid};
use openat_ext::OpenatDirExt;
use ostree_ext::{glib, ostree};
use serde_derive::{Deserialize, Serialize};
use std::collections::btree_map::Entry;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs::{read_dir, File};
use std::io::prelude::*;
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::os::unix::io::{AsRawFd, RawFd};
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::{collections, fs, io};
use tracing::{event, instrument, Level};

use crate::utils;

const INCLUDE_MAXDEPTH: u32 = 50;

/// Path to the flattened JSON serialization of the treefile, installed on the target (client)
/// filesystem.  Nothing actually parses this by default client side today,
/// it's intended to be informative.
const COMPOSE_JSON_PATH: &str = "usr/share/rpm-ostree/treefile.json";

/// Path to client-side treefiles.
const CLIENT_TREEFILES_DIR: &str = "/etc/rpm-ostree/origin.d";

/// This struct holds file descriptors for any external files/data referenced by
/// a TreeComposeConfig.
#[derive(Debug, Default)]
pub(crate) struct TreefileExternals {
    postprocess_script: Option<fs::File>,
    add_files: collections::BTreeMap<String, fs::File>,
    passwd: Option<fs::File>,
    group: Option<fs::File>,
}

// This type name is exposed through ffi.
#[derive(Debug)]
pub struct Treefile {
    // This one isn't used today, but we may do more in the future.
    _workdir: Option<openat::Dir>,
    primary_dfd: openat::Dir,
    #[allow(dead_code)] // Not used in tests
    pub(crate) parsed: TreeComposeConfig,
    serialized: CUtf8Buf,
    pub(crate) externals: TreefileExternals,
}

// We only use this while parsing
struct ConfigAndExternals {
    config: TreeComposeConfig,
    externals: TreefileExternals,
}

/// Parse a YAML treefile definition using base architecture `basearch`.
/// This does not open the externals.
fn treefile_parse_stream<R: io::Read>(
    fmt: utils::InputFormat,
    input: &mut R,
    basearch: Option<&str>,
) -> Result<TreeComposeConfig> {
    let mut treefile: TreeComposeConfig = utils::parse_stream(&fmt, input)?;

    treefile.base.basearch = match (treefile.base.basearch, basearch) {
        (Some(treearch), Some(arch)) => {
            if treearch != arch {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!(
                        "Invalid basearch {} on {}: cross-composes are not supported",
                        treearch, arch
                    ),
                )
                .into());
            } else {
                Some(treearch)
            }
        }
        (None, Some(arch)) => Some(arch.into()),
        // really, only for tests do we not specify a basearch. let's just canonicalize to None
        (_, None) => None,
    };

    // remove from packages-${arch} keys from the extra keys
    let mut archful_pkgs: Option<Vec<String>> = take_archful_pkgs(basearch, &mut treefile)?;

    if fmt == utils::InputFormat::YAML && !treefile.base.extra.is_empty() {
        let keys: Vec<&str> = treefile.base.extra.keys().map(|k| k.as_str()).collect();
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("Unknown fields: {}", keys.join(", ")),
        )
        .into());
    }

    // Special handling for packages, since we allow whitespace within items.
    // We also canonicalize bootstrap_packages to packages here so it's
    // easier to append the basearch packages after.
    let mut pkgs: Vec<String> = vec![];
    {
        if let Some(base_pkgs) = treefile.packages.take() {
            pkgs.extend_from_slice(&whitespace_split_packages(&base_pkgs)?);
        }
        if let Some(bootstrap_pkgs) = treefile.base.bootstrap_packages.take() {
            pkgs.extend_from_slice(&whitespace_split_packages(&bootstrap_pkgs)?);
        }
        if let Some(archful_pkgs) = archful_pkgs.take() {
            pkgs.extend_from_slice(&whitespace_split_packages(&archful_pkgs)?);
        }
    }

    // to be consistent, we also support whitespace-separated modules
    if let Some(mut modules) = treefile.modules.take() {
        if let Some(enable) = modules.enable.take() {
            modules.enable = Some(whitespace_split_packages(&enable)?);
        }
        if let Some(install) = modules.install.take() {
            modules.install = Some(whitespace_split_packages(&install)?);
        }
        treefile.modules = Some(modules);
    }

    if let Some(repo_packages) = treefile.repo_packages.take() {
        treefile.repo_packages = Some(
            repo_packages
                .into_iter()
                .map(|rp| -> Result<RepoPackage> {
                    Ok(RepoPackage {
                        repo: rp.repo,
                        packages: whitespace_split_packages(&rp.packages)?,
                    })
                })
                .collect::<Result<Vec<RepoPackage>>>()?,
        );
    }

    treefile.packages = Some(pkgs);
    treefile = treefile.migrate_legacy_fields()?;
    Ok(treefile)
}

/// Sanity checks that the packages-${basearch} entries are well-formed, and returns the ones
/// matching the current basearch.
fn take_archful_pkgs(
    basearch: Option<&str>,
    treefile: &mut TreeComposeConfig,
) -> Result<Option<Vec<String>>> {
    let mut archful_pkgs: Option<Vec<String>> = None;

    for key in treefile
        .base
        .extra
        .keys()
        .filter(|k| k.starts_with("packages-"))
    {
        if !treefile.base.extra[key].is_array()
            || treefile.base.extra[key]
                .as_array()
                .unwrap()
                .iter()
                .any(|v| !v.is_string())
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Invalid field {}: expected array of strings", key),
            )
            .into());
        }

        if let Some(basearch) = basearch {
            if basearch == &key["packages-".len()..] {
                assert!(archful_pkgs == None);
                archful_pkgs = Some(
                    treefile.base.extra[key]
                        .as_array()
                        .unwrap()
                        .iter()
                        .map(|v| v.as_str().unwrap().into())
                        .collect(),
                );
            }
        }
    }

    // and drop it from the map
    treefile
        .base
        .extra
        .retain(|k, _| !k.starts_with("packages-"));

    Ok(archful_pkgs)
}

/// If a passwd/group file is provided explicitly, load it as a fd.
fn load_passwd_file<P: AsRef<Path>>(basedir: P, cfg: &CheckFile) -> Result<Option<fs::File>> {
    let basedir = basedir.as_ref();
    let file = utils::open_file(basedir.join(&cfg.filename))?;
    Ok(Some(file))
}

type IncludeMap = collections::BTreeMap<(u64, u64), String>;

/// Given a treefile filename and an architecture, parse it and also
/// open its external files.
fn treefile_parse<P: AsRef<Path>>(
    filename: P,
    basearch: Option<&str>,
    seen_includes: &mut IncludeMap,
) -> Result<ConfigAndExternals> {
    let filename = filename.as_ref();
    let f = utils::open_file(filename)?;
    let meta = f.metadata()?;
    let devino = (meta.dev(), meta.ino());
    match seen_includes.entry(devino) {
        Entry::Occupied(_) => bail!(
            "Include loop detected; {} was already included",
            filename.to_str().unwrap()
        ),
        Entry::Vacant(e) => {
            e.insert(filename.to_str().unwrap().to_string());
        }
    };
    let mut f = io::BufReader::new(f);
    let fmt = utils::InputFormat::detect_from_filename(filename)?;
    let tf = treefile_parse_stream(fmt, &mut f, basearch).map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("Parsing {}: {}", filename.to_string_lossy(), e.to_string()),
        )
    })?;
    let postprocess_script = if let Some(ref postprocess) = tf.base.postprocess_script.as_ref() {
        Some(utils::open_file(filename.with_file_name(postprocess))?)
    } else {
        None
    };
    let mut add_files: BTreeMap<String, fs::File> = BTreeMap::new();
    if let Some(add_file_names) = tf.base.add_files.as_ref() {
        for (name, _) in add_file_names.iter() {
            add_files.insert(
                name.clone(),
                utils::open_file(filename.with_file_name(name))?,
            );
        }
    }
    let parent = utils::parent_dir(filename).unwrap();
    let passwd = match tf.get_check_passwd() {
        CheckPasswd::File(ref f) => load_passwd_file(&parent, f)?,
        _ => None,
    };
    let group = match tf.get_check_groups() {
        CheckGroups::File(ref f) => load_passwd_file(&parent, f)?,
        _ => None,
    };

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

/// Merge a map field similarly to Python's `dict.update()`. In case of
/// duplicate keys, `dest` wins (`src` is the "included" config).
fn merge_map_field<T>(
    dest: &mut Option<BTreeMap<String, T>>,
    src: &mut Option<BTreeMap<String, T>>,
) {
    if let Some(mut srcv) = src.take() {
        if let Some(mut destv) = dest.take() {
            srcv.append(&mut destv);
        }
        *dest = Some(srcv);
    }
}

/// Merge an hashset field by extending.
fn merge_hashset_field<T: Eq + std::hash::Hash>(
    dest: &mut Option<HashSet<T>>,
    src: &mut Option<HashSet<T>>,
) {
    if let Some(mut srcv) = src.take() {
        if let Some(destv) = dest.take() {
            srcv.extend(destv);
        }
        *dest = Some(srcv);
    }
}

/// Merge modules fields.
pub(crate) fn merge_modules(dest: &mut Option<ModulesConfig>, src: &mut Option<ModulesConfig>) {
    if let Some(mut srcv) = src.take() {
        if let Some(mut destv) = dest.take() {
            merge_vec_field(&mut destv.enable, &mut srcv.enable);
            merge_vec_field(&mut destv.install, &mut srcv.install);
            srcv = destv;
        }
        *dest = Some(srcv);
    }
}

/// Given two configs, merge them.
fn treefile_merge(dest: &mut TreeComposeConfig, src: &mut TreeComposeConfig) {
    macro_rules! merge_basics {
        ( $($field:ident),* ) => {{
            $( merge_basic_field(&mut dest.base.$field, &mut src.base.$field); )*
        }};
    }
    macro_rules! merge_hashsets {
        ( $($field:ident),* ) => {{
            $( merge_hashset_field(&mut dest.base.$field, &mut src.base.$field); )*
        }};
    }
    macro_rules! merge_maps {
        ( $($field:ident),* ) => {{
            $( merge_map_field(&mut dest.base.$field, &mut src.base.$field); )*
        }};
    }
    macro_rules! merge_vecs {
        ( $($field:ident),* ) => {{
            $( merge_vec_field(&mut dest.base.$field, &mut src.base.$field); )*
        }};
    }

    merge_basics!(
        treeref,
        basearch,
        rojig,
        selinux,
        gpg_key,
        include,
        container,
        recommends,
        readonly_executables,
        documentation,
        boot_location,
        tmp_is_dir,
        default_target,
        machineid_compat,
        releasever,
        automatic_version_prefix,
        automatic_version_suffix,
        rpmdb,
        mutate_os_release,
        preserve_passwd,
        check_passwd,
        check_groups,
        postprocess_script,
        rpmdb_normalize
    );
    merge_hashsets!(ignore_removed_groups, ignore_removed_users);
    merge_maps!(add_commit_metadata);
    merge_vecs!(
        repos,
        lockfile_repos,
        bootstrap_packages,
        exclude_packages,
        ostree_layers,
        ostree_override_layers,
        install_langs,
        initramfs_args,
        units,
        etc_group_members,
        postprocess,
        add_files,
        remove_files,
        remove_from_packages
    );

    merge_vec_field(&mut dest.packages, &mut src.packages);
    merge_vec_field(&mut dest.repo_packages, &mut src.repo_packages);
    merge_basic_field(&mut dest.cliwrap, &mut src.cliwrap);
    merge_basic_field(&mut dest.derive.base_refspec, &mut src.derive.base_refspec);
    merge_modules(&mut dest.modules, &mut src.modules);
}

/// Merge the treefile externals. There are currently only two keys that
/// reference external files.
fn treefile_merge_externals(dest: &mut TreefileExternals, src: &mut TreefileExternals) {
    // This one, being a basic-valued field, has first-wins semantics.
    if dest.postprocess_script.is_none() {
        dest.postprocess_script = src.postprocess_script.take();
    }

    // add-files is an array and hence has append semantics.
    dest.add_files.append(&mut src.add_files);

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
    basearch: Option<&str>,
    depth: u32,
    seen_includes: &mut IncludeMap,
) -> Result<ConfigAndExternals> {
    let filename = filename.as_ref();
    let mut parsed = treefile_parse(filename, basearch, seen_includes)?;
    let include = parsed
        .config
        .base
        .include
        .take()
        .unwrap_or_else(|| Include::Multiple(Vec::new()));
    let mut includes = match include {
        Include::Single(v) => vec![v],
        Include::Multiple(v) => v,
    };
    if let Some(mut arch_includes) = parsed.config.base.arch_include.take() {
        if let Some(basearch) = basearch {
            if let Some(arch_include_value) = arch_includes.remove(basearch) {
                match arch_include_value {
                    Include::Single(v) => includes.push(v),
                    Include::Multiple(v) => includes.extend(v),
                }
            }
        }
    }
    for include_path in includes.iter() {
        if depth == INCLUDE_MAXDEPTH {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Reached maximum include depth {}", INCLUDE_MAXDEPTH),
            )
            .into());
        }
        let parent = utils::parent_dir(filename).unwrap();
        let include_path = parent.join(include_path);
        let mut included =
            treefile_parse_recurse(include_path, basearch, depth + 1, seen_includes)?;
        treefile_merge(&mut parsed.config, &mut included.config);
        treefile_merge_externals(&mut parsed.externals, &mut included.externals);
    }
    Ok(parsed)
}

/// Recursively parse a treefile, merging along the way, and finally postprocess it (e.g. collapse
/// repo-packages duplicates, substitute variables, validate the config).
fn treefile_parse_and_process<P: AsRef<Path>>(
    filename: P,
    basearch: Option<&str>,
) -> Result<ConfigAndExternals> {
    let mut seen_includes = collections::BTreeMap::new();
    let mut parsed = treefile_parse_recurse(filename, basearch, 0, &mut seen_includes)?;
    event!(Level::DEBUG, "parsed successfully");
    parsed.config.handle_repo_packages_overrides();
    parsed.config = parsed.config.substitute_vars()?;
    Treefile::validate_config(&parsed.config)?;
    Ok(parsed)
}

// Similar to the importer check but just checks for prefixes since
// they're files, and also allows /etc since it's before conversion
fn add_files_path_is_valid(path: &str) -> bool {
    let path = path.trim_start_matches('/');
    (path.starts_with("usr/") && !path.starts_with("usr/local/"))
        || path.starts_with("etc/")
        || path.starts_with("bin/")
        || path.starts_with("sbin/")
        || path.starts_with("lib/")
        || path.starts_with("lib64/")
}

impl Treefile {
    /// The main treefile creation entrypoint.
    #[instrument(skip(workdir))]
    fn new_boxed(
        filename: &Path,
        basearch: Option<&str>,
        workdir: Option<openat::Dir>,
    ) -> Result<Box<Treefile>> {
        let parsed = treefile_parse_and_process(filename, basearch)?;
        let dfd = openat::Dir::open(utils::parent_dir(filename).unwrap())?;
        let serialized = Treefile::serialize_json_string(&parsed.config)?;
        Ok(Box::new(Treefile {
            primary_dfd: dfd,
            parsed: parsed.config,
            _workdir: workdir,
            serialized,
            externals: parsed.externals,
        }))
    }

    pub(crate) fn new_from_string(buf: &str) -> Result<Box<Self>> {
        let mut treefile =
            treefile_parse_stream(utils::InputFormat::JSON, &mut buf.as_bytes(), None)?;
        treefile = treefile.substitute_vars()?;
        let serialized = CUtf8Buf::from(buf);
        let td = tempfile::tempdir()?;
        let primary_dfd = openat::Dir::open(td.path())?;
        Ok(Box::new(Treefile {
            primary_dfd,
            parsed: treefile,
            _workdir: None,
            serialized,
            externals: Default::default(),
        }))
    }

    pub(crate) fn new_from_config(
        parsed: TreeComposeConfig,
        cfgdir: Option<&openat::Dir>,
    ) -> Result<Self> {
        let serialized = Treefile::serialize_json_string(&parsed)?;
        let primary_dfd = if let Some(d) = cfgdir {
            d.sub_dir(".")?
        } else {
            // If we weren't passed a configdir, for now we just make a tempdir
            // then delete it, holding open a useless fd to it.  This is to
            // avoid changing all of the treefile code to use an Option<> for the dfd right now.
            let td = tempfile::tempdir()?;
            openat::Dir::open(td.path())?
        };
        Ok(Treefile {
            primary_dfd,
            parsed,
            _workdir: None,
            serialized,
            externals: Default::default(),
        })
    }

    /// Return the raw file descriptor for the workdir
    pub(crate) fn get_workdir(&self) -> i32 {
        self.primary_dfd.as_raw_fd()
    }

    /// Return the raw file descriptor for the postprocess script
    pub(crate) fn get_postprocess_script(&mut self) -> Option<&mut File> {
        self.externals.postprocess_script.as_mut()
    }

    /// Access the opened file object for the injected file
    pub(crate) fn get_add_file(&mut self, filename: &str) -> &mut File {
        self.externals
            .add_files
            .get_mut(filename)
            .expect("add-file")
    }

    /// Returns the "ref" entry in treefile, or the empty string if unset.
    pub(crate) fn get_ostree_ref(&self) -> String {
        self.parsed.base.treeref.clone().unwrap_or_default()
    }

    pub(crate) fn get_passwd_fd(&mut self) -> i32 {
        self.externals.passwd.as_mut().map_or(-1, raw_seeked_fd)
    }

    pub(crate) fn get_group_fd(&mut self) -> i32 {
        self.externals.group.as_mut().map_or(-1, raw_seeked_fd)
    }

    pub(crate) fn get_json_string(&self) -> String {
        self.serialized.to_string()
    }

    pub(crate) fn get_ostree_layers(&self) -> Vec<String> {
        self.parsed.base.ostree_layers.clone().unwrap_or_default()
    }

    pub(crate) fn get_ostree_override_layers(&self) -> Vec<String> {
        self.parsed
            .base
            .ostree_override_layers
            .clone()
            .unwrap_or_default()
    }

    pub(crate) fn get_all_ostree_layers(&self) -> Vec<String> {
        self.get_ostree_layers()
            .into_iter()
            .chain(self.get_ostree_override_layers())
            .collect()
    }

    pub(crate) fn get_packages(&self) -> Vec<String> {
        self.parsed.packages.clone().unwrap_or_default()
    }

    pub(crate) fn get_packages_local(&self) -> Vec<String> {
        self.parsed
            .derive
            .packages_local
            .iter()
            .flatten()
            .map(|(k, v)| format!("{}:{}", v, k))
            .collect()
    }

    pub(crate) fn get_packages_local_fileoverride(&self) -> Vec<String> {
        self.parsed
            .derive
            .packages_local_fileoverride
            .iter()
            .flatten()
            .map(|(k, v)| format!("{}:{}", v, k))
            .collect()
    }

    pub(crate) fn get_modules_enable(&self) -> Vec<String> {
        self.parsed
            .modules
            .as_ref()
            .map(|m| m.enable.as_ref())
            .flatten()
            .cloned()
            .into_iter()
            .flatten()
            .collect()
    }

    pub(crate) fn get_modules_install(&self) -> Vec<String> {
        self.parsed
            .modules
            .as_ref()
            .map(|m| m.install.as_ref())
            .flatten()
            .cloned()
            .into_iter()
            .flatten()
            .collect()
    }

    pub(crate) fn get_packages_override_remove(&self) -> Vec<String> {
        self.parsed
            .derive
            .override_remove
            .clone()
            .unwrap_or_default()
    }

    pub(crate) fn get_packages_override_replace_local(&self) -> Vec<String> {
        self.parsed
            .derive
            .override_replace_local
            .iter()
            .flatten()
            .map(|(k, v)| format!("{}:{}", v, k))
            .collect()
    }

    pub(crate) fn get_exclude_packages(&self) -> Vec<String> {
        self.parsed
            .base
            .exclude_packages
            .clone()
            .unwrap_or_default()
    }

    pub(crate) fn get_platform_module(&self) -> String {
        self.parsed.base.platform_module.clone().unwrap_or_default()
    }

    pub(crate) fn get_install_langs(&self) -> Vec<String> {
        self.parsed.base.install_langs.clone().unwrap_or_default()
    }

    /// If install_langs is set, generate a value suitable for the RPM macro `_install_langs`;
    /// otherwise return the empty string.
    pub(crate) fn format_install_langs_macro(&self) -> String {
        if let Some(langs) = self.parsed.base.install_langs.as_ref() {
            langs.join(":")
        } else {
            "".to_string()
        }
    }

    pub(crate) fn get_repos(&self) -> Vec<String> {
        self.parsed.base.repos.clone().unwrap_or_default()
    }

    pub(crate) fn get_lockfile_repos(&self) -> Vec<String> {
        self.parsed.base.lockfile_repos.clone().unwrap_or_default()
    }

    pub(crate) fn get_ref(&self) -> &str {
        self.parsed.base.treeref.as_deref().unwrap_or_default()
    }

    pub(crate) fn get_cliwrap(&self) -> bool {
        self.parsed.cliwrap.unwrap_or(false)
    }

    pub(crate) fn get_readonly_executables(&self) -> bool {
        self.parsed.base.readonly_executables.unwrap_or(false)
    }

    pub(crate) fn get_documentation(&self) -> bool {
        self.parsed.base.documentation.unwrap_or(true)
    }

    pub(crate) fn get_recommends(&self) -> bool {
        self.parsed.base.recommends.unwrap_or(true)
    }

    pub(crate) fn get_selinux(&self) -> bool {
        self.parsed.base.selinux.unwrap_or(true)
    }

    pub(crate) fn get_releasever(&self) -> &str {
        self.parsed.base.releasever.as_deref().unwrap_or_default()
    }

    /// Returns true if the database backend must be regenerated using the target system.
    pub(crate) fn rpmdb_backend_is_target(&self) -> bool {
        self.parsed
            .base
            .rpmdb
            .as_ref()
            .map_or(true, |b| *b != RpmdbBackend::Host)
    }

    pub(crate) fn should_normalize_rpmdb(&self) -> bool {
        self.parsed.base.rpmdb_normalize.unwrap_or(false)
    }

    pub(crate) fn get_files_remove_regex(&self, package: &str) -> Vec<String> {
        let mut files_to_remove: Vec<String> = Vec::new();
        if let Some(ref packages) = self.parsed.base.remove_from_packages {
            for pkg in packages {
                if pkg[0] == package {
                    files_to_remove.extend_from_slice(&pkg[1..]);
                }
            }
        }
        files_to_remove
    }

    pub(crate) fn get_repo_packages(&self) -> &[RepoPackage] {
        self.parsed.repo_packages.as_deref().unwrap_or_default()
    }

    pub(crate) fn clear_repo_packages(&mut self) {
        self.parsed.repo_packages.take();
    }

    /// Do some upfront semantic checks we can do beyond just the type safety serde provides.
    fn validate_config(config: &TreeComposeConfig) -> Result<()> {
        // check add-files
        if let Some(files) = &config.base.add_files {
            for (_, dest) in files.iter() {
                if !add_files_path_is_valid(dest) {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        format!("Unsupported path in add-files: {}", dest),
                    )
                    .into());
                }
            }
        }
        if let Some(version_suffix) = config.base.automatic_version_suffix.as_ref() {
            if !(version_suffix.len() == 1 && version_suffix.is_ascii()) {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!(
                        "Invalid automatic-version-suffix, must be exactly one ASCII character: {}",
                        version_suffix
                    ),
                )
                .into());
            }
        }
        Ok(())
    }

    /// Do some upfront semantic checks we can do beyond just the type safety serde provides.
    fn validate_base_config(config: &TreeComposeConfig) -> Result<()> {
        if config.base.repos.is_none() && config.base.lockfile_repos.is_none() {
            return Err(anyhow!(
                r#"Treefile has neither "repos" nor "lockfile-repos""#
            ));
        }
        Ok(())
    }

    fn serialize_json_string(config: &TreeComposeConfig) -> Result<CUtf8Buf> {
        let output = serde_json::to_string_pretty(config)?;
        Ok(CUtf8Buf::from_string(output))
    }

    /// Throw an error if any derive fields are set.
    pub(crate) fn error_if_deriving(&self) -> Result<()> {
        self.parsed.derive.error_if_nonempty()
    }

    /// Throw an error if any base fields are set.
    pub(crate) fn error_if_base(&self) -> Result<()> {
        self.parsed.base.error_if_nonempty()
    }

    /// Pretty-print treefile content as JSON to stdout.
    pub fn prettyprint_json_stdout(&self) {
        std::io::stdout()
            .write_all(self.serialized.as_bytes())
            .unwrap();
    }

    /// Given a treefile, print warnings about items which are deprecated.
    pub(crate) fn print_deprecation_warnings(&self) {
        let mut deprecated = false;
        match self
            .parsed
            .base
            .boot_location
            .as_ref()
            .copied()
            .unwrap_or_default()
        {
            BootLocation::Modules => {}
            o => {
                let s = serde_json::to_string(&o).expect("serialize");
                deprecated = true;
                eprintln!(
                    "warning: boot-location: {} is deprecated, use boot-location: modules",
                    s
                )
            }
        }
        if deprecated {
            std::thread::sleep(std::time::Duration::from_secs(3));
        }
    }

    /// Given a treefile, print notices about items which are experimental.
    pub(crate) fn print_experimental_notices(&self) {
        print_experimental_notice(self.parsed.modules.is_some(), "modules");
        print_experimental_notice(self.parsed.base.lockfile_repos.is_some(), "lockfile-repos");
    }

    pub(crate) fn get_checksum(
        &self,
        mut repo: Pin<&mut crate::ffi::OstreeRepo>,
    ) -> CxxResult<String> {
        let repo = &repo.gobj_wrap();
        // Notice we hash the *reserialization* of the final flattened treefile only so that e.g.
        // comments/whitespace/hash table key reorderings don't trigger a respin. We could take
        // this further by using a custom `serialize_with` for Vecs where ordering doesn't matter
        // (or just sort the Vecs).
        let mut hasher = glib::Checksum::new(glib::ChecksumType::Sha256).unwrap();
        self.parsed.hasher_update(&mut hasher)?;
        self.externals.hasher_update(&mut hasher)?;

        let it = self.parsed.base.ostree_layers.iter().flat_map(|x| x.iter());
        let it = it.chain(
            self.parsed
                .base
                .ostree_override_layers
                .iter()
                .flat_map(|x| x.iter()),
        );

        for v in it {
            let rev = repo.resolve_rev(v, false)?.unwrap();
            let rev = rev.as_str();
            let (commit, _) = repo.load_commit(rev)?;
            let content_checksum =
                ostree::commit_get_content_checksum(&commit).expect("content checksum");
            let content_checksum = content_checksum.as_str();
            hasher.update(content_checksum.as_bytes());
        }
        Ok(hasher.string().expect("hash"))
    }

    /// Perform sanity checks on externally provided input, such
    /// as the executability of `postprocess-script`.
    pub(crate) fn sanitycheck_externals(&self) -> Result<()> {
        if let Some(script) = self.externals.postprocess_script.as_ref() {
            let mode = script.metadata()?.permissions().mode();
            if mode & 0o111 == 0 {
                return Err(anyhow!("postprocess-script must be executable"));
            }
        }

        let parsed = &self.parsed;
        let machineid_compat = parsed.base.machineid_compat.unwrap_or(true);
        let n_units = parsed
            .base
            .units
            .as_ref()
            .map(|v| v.len())
            .unwrap_or_default();
        if !machineid_compat && n_units > 0 {
            return Err(anyhow!(
                "'units' directive is incompatible with machineid-compat = false"
            ));
        }

        Ok(())
    }

    /// Write the serialized treefile into /usr/share on the target filesystem.
    pub(crate) fn write_compose_json(&self, rootfs_dfd: &openat::Dir) -> Result<()> {
        let target = Path::new(COMPOSE_JSON_PATH);
        rootfs_dfd.ensure_dir_all(target.parent().unwrap(), 0o755)?;
        rootfs_dfd.write_file_contents(target, 0o644, self.serialized.as_bytes())?;
        Ok(())
    }
}

fn print_experimental_notice(print: bool, key: &str) {
    if print {
        eprintln!(
            "NOTICE: treefile key `{}` is experimental and subject to change\n",
            key
        );
    }
}

impl RepoPackage {
    pub(crate) fn get_repo(&self) -> &str {
        self.repo.as_str()
    }

    pub(crate) fn get_packages(&self) -> &[String] {
        self.packages.as_slice()
    }
}

fn hash_file(hasher: &mut glib::Checksum, mut f: &fs::File) -> Result<()> {
    let mut reader = io::BufReader::with_capacity(128 * 1024, f);
    loop {
        // have to scope fill_buf() so we can consume() below
        let n = {
            let buf = reader.fill_buf()?;
            hasher.update(buf);
            buf.len()
        };
        if n == 0 {
            break;
        }
        reader.consume(n);
    }
    f.seek(io::SeekFrom::Start(0))?;
    Ok(())
}

impl TreefileExternals {
    pub(crate) fn group_file_mut(&mut self, _sentinel: &CheckFile) -> Result<&mut fs::File> {
        let group_file = self
            .group
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("missing group file"))?;
        group_file.seek(io::SeekFrom::Start(0))?;
        Ok(group_file)
    }

    pub(crate) fn passwd_file_mut(&mut self, _sentinel: &CheckFile) -> Result<&mut fs::File> {
        let passwd_file = self
            .passwd
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("missing passwd file"))?;
        passwd_file.seek(io::SeekFrom::Start(0))?;
        Ok(passwd_file)
    }

    fn hasher_update(&self, hasher: &mut glib::Checksum) -> Result<()> {
        if let Some(ref f) = self.postprocess_script {
            hash_file(hasher, f)?;
        }
        if let Some(ref f) = self.passwd {
            hash_file(hasher, f)?;
        }
        if let Some(ref f) = self.group {
            hash_file(hasher, f)?;
        }
        for f in self.add_files.values() {
            hash_file(hasher, f)?;
        }
        Ok(())
    }

    fn assert_nonempty(&self) {
        // can't use the Default trick here because we can't auto-derive Eq because of `File`
        assert!(self.postprocess_script.is_none());
        assert!(self.add_files.is_empty());
        assert!(self.passwd.is_none());
        assert!(self.group.is_none());
    }
}

/// For increased readability in YAML/JSON, we support whitespace in individual
/// array elements.
fn whitespace_split_packages(pkgs: &[String]) -> Result<Vec<String>> {
    let mut ret = vec![];
    for element in pkgs.iter() {
        ret.extend(split_whitespace_unless_quoted(element)?.map(String::from));
    }

    Ok(ret)
}

// Helper for whitespace_split_packages().
// Splits a String by whitespace unless substring is wrapped between single quotes
// and returns split &str in an Iterator.
fn split_whitespace_unless_quoted(element: &str) -> Result<impl Iterator<Item = &str>> {
    let mut ret = vec![];
    let mut start_index = 0;
    let mut looping_over_quoted_pkg = false;
    for (i, c) in element.chars().enumerate() {
        if c == '\'' {
            if looping_over_quoted_pkg {
                ret.push(&element[start_index..i]);
                looping_over_quoted_pkg = false;
            } else {
                ret.extend((&element[start_index..i]).split_whitespace());
                looping_over_quoted_pkg = true;
            }
            start_index = i + 1;
        }
        if i == element.len() - 1 {
            if looping_over_quoted_pkg {
                bail!("Missing terminating quote: {}", element);
            }
            ret.extend((&element[start_index..]).split_whitespace());
        }
    }

    Ok(ret.into_iter())
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum BootLocation {
    #[serde(rename = "new")]
    New,
    #[serde(rename = "modules")]
    Modules,
}

impl Default for BootLocation {
    fn default() -> Self {
        BootLocation::New
    }
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
#[serde(tag = "type")]
pub(crate) enum CheckGroups {
    #[serde(rename = "none")]
    None,
    #[serde(rename = "previous")]
    Previous,
    #[serde(rename = "file")]
    File(CheckFile),
    #[serde(rename = "data")]
    Data(CheckGroupsData),
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
pub(crate) struct CheckFile {
    filename: String,
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
pub(crate) struct CheckGroupsData {
    pub(crate) entries: BTreeMap<String, u32>,
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
#[serde(tag = "type")]
pub(crate) enum CheckPasswd {
    #[serde(rename = "none")]
    None,
    #[serde(rename = "previous")]
    Previous,
    #[serde(rename = "file")]
    File(CheckFile),
    #[serde(rename = "data")]
    Data(CheckPasswdData),
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
pub(crate) struct CheckPasswdData {
    pub(crate) entries: BTreeMap<String, CheckPasswdDataEntries>,
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
#[serde(untagged)]
pub(crate) enum CheckPasswdDataEntries {
    IdValue(u32),
    IdTuple([u32; 1]),
    UidGid((u32, u32)),
}

impl CheckPasswdDataEntries {
    /// Return IDs for user and group.
    pub fn ids(&self) -> (Uid, Gid) {
        let (user, group) = match self {
            CheckPasswdDataEntries::IdValue(v) => (*v, *v),
            CheckPasswdDataEntries::IdTuple([v]) => (*v, *v),
            CheckPasswdDataEntries::UidGid(v) => *v,
        };
        (Uid::from_raw(user), Gid::from_raw(group))
    }
}

impl From<u32> for CheckPasswdDataEntries {
    fn from(item: u32) -> Self {
        Self::IdValue(item)
    }
}

impl From<[u32; 1]> for CheckPasswdDataEntries {
    fn from(item: [u32; 1]) -> Self {
        Self::IdTuple(item)
    }
}

impl From<(u32, u32)> for CheckPasswdDataEntries {
    fn from(item: (u32, u32)) -> Self {
        Self::UidGid(item)
    }
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
pub(crate) struct Rojig {
    pub(crate) name: String,
    pub(crate) summary: String,
    pub(crate) license: String,
    pub(crate) description: Option<String>,
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
#[serde(untagged)]
pub(crate) enum Include {
    Single(String),
    Multiple(Vec<String>),
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
/// The database backend; see https://github.com/coreos/fedora-coreos-tracker/issues/609
/// and https://fedoraproject.org/wiki/Changes/Sqlite_Rpmdb
pub(crate) enum RpmdbBackend {
    // We messed up the name originally, allow this for backcompat.  xref https://github.com/openshift/os/issues/552
    #[serde(alias = "b-d-b")]
    Bdb,
    Sqlite,
    Ndb,
    Target,
    Host,
}

// Because of how we handle includes, *everything* here has to be
// Option<T>.  The defaults live in the code (e.g. machineid-compat defaults
// to `true`).
/// Things that live *directly* in this struct are in common to both the base compose and derive
/// cases. Everything else is specific to either case and so lives in their respective flattened
/// field.
#[derive(Serialize, Deserialize, Debug, Default, PartialEq, Eq)]
pub(crate) struct TreeComposeConfig {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) packages: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "repo-packages")]
    pub(crate) repo_packages: Option<Vec<RepoPackage>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) modules: Option<ModulesConfig>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) cliwrap: Option<bool>,

    #[serde(flatten)]
    pub(crate) base: BaseComposeConfigFields,

    #[serde(flatten)]
    pub(crate) derive: DeriveConfigFields,
}

/// These fields are only useful when composing a new ostree commit.
#[derive(Serialize, Deserialize, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub(crate) struct BaseComposeConfigFields {
    // Compose controls
    #[serde(rename = "ref")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) treeref: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) basearch: Option<String>,
    // Optional rojig data
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) rojig: Option<Rojig>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) repos: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "lockfile-repos")]
    pub(crate) lockfile_repos: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) selinux: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "gpg-key")]
    pub(crate) gpg_key: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) include: Option<Include>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "arch-include")]
    pub(crate) arch_include: Option<BTreeMap<String, Include>>,

    // Core content
    // Deprecated option
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) bootstrap_packages: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ostree-layers")]
    pub(crate) ostree_layers: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ostree-override-layers")]
    pub(crate) ostree_override_layers: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "exclude-packages")]
    pub(crate) exclude_packages: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "platform-module")]
    pub(crate) platform_module: Option<String>,

    // Content installation opts
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) container: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) recommends: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) documentation: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "install-langs")]
    pub(crate) install_langs: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "initramfs-args")]
    pub(crate) initramfs_args: Option<Vec<String>>,
    #[serde(rename = "readonly-executables")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) readonly_executables: Option<bool>,

    // Tree layout options
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "boot-location")]
    pub(crate) boot_location: Option<BootLocation>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "tmp-is-dir")]
    pub(crate) tmp_is_dir: Option<bool>,

    // systemd
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) units: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "default-target")]
    pub(crate) default_target: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "machineid-compat")]
    // Defaults to `true`
    pub(crate) machineid_compat: Option<bool>,

    // versioning
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) releasever: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "automatic-version-prefix")]
    pub(crate) automatic_version_prefix: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "automatic-version-suffix")]
    pub(crate) automatic_version_suffix: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "mutate-os-release")]
    pub(crate) mutate_os_release: Option<String>,

    // passwd-related bits
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "etc-group-members")]
    pub(crate) etc_group_members: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "preserve-passwd")]
    pub(crate) preserve_passwd: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "check-passwd")]
    pub(crate) check_passwd: Option<CheckPasswd>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "check-groups")]
    pub(crate) check_groups: Option<CheckGroups>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ignore-removed-users")]
    pub(crate) ignore_removed_users: Option<HashSet<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ignore-removed-groups")]
    pub(crate) ignore_removed_groups: Option<HashSet<String>>,

    // Content manipulation
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "postprocess-script")]
    // This one references an external filename
    pub(crate) postprocess_script: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    // This one is inline, and supports multiple (hence is useful for inheritance)
    pub(crate) postprocess: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "add-files")]
    pub(crate) add_files: Option<Vec<(String, String)>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "remove-files")]
    pub(crate) remove_files: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "remove-from-packages")]
    pub(crate) remove_from_packages: Option<Vec<Vec<String>>>,
    // The BTreeMap here is on purpose; it ensures we always re-serialize in sorted order so that
    // checksumming is deterministic across runs. (And serde itself uses BTreeMap for child objects
    // as well).
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "add-commit-metadata")]
    pub(crate) add_commit_metadata: Option<BTreeMap<String, serde_json::Value>>,
    // The database backend
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) rpmdb: Option<RpmdbBackend>,

    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "rpmdb-normalize")]
    pub(crate) rpmdb_normalize: Option<bool>,

    #[serde(flatten)]
    pub(crate) legacy_fields: LegacyTreeComposeConfigFields,

    // This is used to support `packages-${arch}` keys. For YAML files, any other keys cause an
    // error. For JSON files, unknown keys are silently ignored.
    #[serde(flatten)]
    pub(crate) extra: HashMap<String, serde_json::Value>,
}

#[derive(Serialize, Deserialize, Debug, Default, PartialEq, Eq)]
pub(crate) struct RepoPackage {
    pub(crate) repo: String,
    pub(crate) packages: Vec<String>,
}

#[derive(Serialize, Deserialize, Debug, Default, PartialEq, Eq, Clone)]
pub(crate) struct ModulesConfig {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) enable: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) install: Option<Vec<String>>,
}

#[derive(Serialize, Deserialize, Debug, Default, PartialEq, Eq)]
pub(crate) struct LegacyTreeComposeConfigFields {
    #[serde(skip_serializing)]
    pub(crate) gpg_key: Option<String>,
    #[serde(skip_serializing)]
    pub(crate) boot_location: Option<BootLocation>,
    #[serde(skip_serializing)]
    pub(crate) default_target: Option<String>,
    #[serde(skip_serializing)]
    pub(crate) automatic_version_prefix: Option<String>,
}

#[derive(Serialize, Deserialize, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub(crate) struct DeriveCustom {
    pub(crate) url: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) description: Option<String>,
}

#[derive(Serialize, Deserialize, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub(crate) struct DeriveInitramfs {
    pub(crate) regenerate: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) etc: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) args: Option<Vec<String>>,
}

/// These fields are only useful when deriving from a prior ostree commit;
/// at the moment we only use them when translating an origin file
/// to a treefile for client side assembly.
#[derive(Serialize, Deserialize, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub(crate) struct DeriveConfigFields {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) base_refspec: Option<String>,

    // Packages
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) packages_local: Option<BTreeMap<String, String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) packages_local_fileoverride: Option<BTreeMap<String, String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) override_remove: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) override_replace_local: Option<BTreeMap<String, String>>,

    // Initramfs
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) initramfs: Option<DeriveInitramfs>,

    // Custom origin
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) custom: Option<DeriveCustom>,

    // Misc
    pub(crate) override_commit: Option<String>,
}

impl BaseComposeConfigFields {
    pub(crate) fn error_if_nonempty(&self) -> Result<()> {
        // exemption for `basearch`, which we set to the current arch during parsing
        let s = Self {
            basearch: self.basearch.clone(),
            ..Default::default()
        };
        if &s != self {
            let j = serde_json::to_string_pretty(self)?;
            bail!("the following base fields are not supported:\n{}", j);
        }
        Ok(())
    }
}

impl DeriveConfigFields {
    pub(crate) fn error_if_nonempty(&self) -> Result<()> {
        if &Self::default() != self {
            let j = serde_json::to_string_pretty(self)?;
            bail!("the following derivation fields are not supported:\n{}", j);
        }
        Ok(())
    }
}

impl TreeComposeConfig {
    /// Look for use of legacy/renamed fields and migrate them to the new field.
    fn migrate_legacy_fields(mut self) -> Result<Self> {
        macro_rules! migrate_field {
            ( $field:ident ) => {{
                if self.base.legacy_fields.$field.is_some() && self.base.$field.is_some() {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        format!("Cannot use new and legacy forms of {}", stringify!($field)),
                    )
                    .into());
                }
                self.base.$field = self.base.$field.or(self.base.legacy_fields.$field.take());
            }};
        }

        migrate_field!(gpg_key);
        migrate_field!(boot_location);
        migrate_field!(default_target);
        migrate_field!(automatic_version_prefix);

        Ok(self)
    }

    /// Look for use of ${variable} and replace it by its proper value
    fn substitute_vars(mut self) -> Result<Self> {
        let mut substvars: collections::HashMap<String, String> = collections::HashMap::new();
        // Substitute ${basearch} and ${releasever}
        if let Some(arch) = &self.base.basearch {
            substvars.insert("basearch".to_string(), arch.clone());
        }
        if let Some(releasever) = &self.base.releasever {
            substvars.insert("releasever".to_string(), releasever.clone());
        }
        envsubst::validate_vars(&substvars)?;

        macro_rules! substitute_field {
            ( $field:ident ) => {{
                if let Some(value) = self.base.$field.take() {
                    self.base.$field = if envsubst::is_templated(&value) {
                        match envsubst::substitute(value, &substvars) {
                            Ok(s) => Some(s),
                            Err(e) => return Err(anyhow!(e.to_string())),
                        }
                    } else {
                        Some(value)
                    }
                }
            }};
        }
        substitute_field!(treeref);
        substitute_field!(automatic_version_prefix);
        substitute_field!(mutate_os_release);

        Ok(self)
    }

    fn hasher_update(&self, hasher: &mut glib::Checksum) -> Result<()> {
        // don't use pretty mode to increase the chances of a stable serialization
        // https://github.com/projectatomic/rpm-ostree/pull/1865
        hasher.update(serde_json::to_vec(self)?.as_slice());
        Ok(())
    }

    pub(crate) fn get_check_passwd(&self) -> &CheckPasswd {
        static DEFAULT: CheckPasswd = CheckPasswd::Previous;
        self.base.check_passwd.as_ref().unwrap_or(&DEFAULT)
    }

    pub(crate) fn get_check_groups(&self) -> &CheckGroups {
        static DEFAULT: CheckGroups = CheckGroups::Previous;
        self.base.check_groups.as_ref().unwrap_or(&DEFAULT)
    }

    // we need to ensure that appended repo packages override earlier ones
    fn handle_repo_packages_overrides(&mut self) {
        if let Some(repo_packages) = self.repo_packages.as_mut() {
            let mut seen_pkgs: HashSet<String> = HashSet::new();
            // Create a temporary new filtered vec; see
            // https://doc.rust-lang.org/std/iter/struct.Map.html#notes-about-side-effects for why
            // the reverse and re-reverse due to the side effect during `map()`.
            let mut v: Vec<_> = repo_packages
                .drain(..)
                .rev()
                .map(|mut rp| {
                    rp.packages.retain(|p| seen_pkgs.insert(p.into()));
                    rp
                })
                .filter(|rp| !rp.packages.is_empty())
                .collect();
            // Now replace the original, re-reversing.
            v.reverse();
            *repo_packages = v;
        }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use indoc::indoc;
    use openat_ext::OpenatDirExt;
    use tempfile;

    pub(crate) static ARCH_X86_64: &str = "x86_64";

    pub(crate) static VALID_PRELUDE: &str = indoc! {r#"
        ref: "exampleos/x86_64/blah"
        repos:
         - baserepo
        packages:
         - foo bar
         - baz
         - corge 'quuz >= 1.0'
        packages-x86_64:
         - grub2 grub2-tools
        packages-s390x:
         - zipl
        repo-packages:
            - repo: baserepo
              packages:
                - blah bloo
        modules:
           enable:
             - foobar:2.0
           install:
             - nodejs:15
             - swig:3.0/complete sway:rolling
    "#};

    // This one has "comments" (hence unknown keys)
    pub(crate) static VALID_PRELUDE_JS: &str = indoc! {r#"
        {
         "ref": "exampleos/${basearch}/blah",
         "comment-packages": "We want baz to enable frobnication",
         "repos": ["baserepo"],
         "packages": ["foo", "bar", "baz"],
         "packages-x86_64": ["grub2", "grub2-tools"],
         "comment-packages-s390x": "Note that s390x uses its own bootloader",
         "packages-s390x": ["zipl"]
        }
    "#};

    #[test]
    fn from_string() {
        let _ = Treefile::new_from_string("{}").unwrap();
    }

    #[test]
    fn basic_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE.as_bytes());
        let mut treefile =
            treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        treefile = treefile.substitute_vars().unwrap();
        assert!(treefile.base.treeref.unwrap() == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 7);
        assert_eq!(
            treefile.repo_packages,
            Some(vec![RepoPackage {
                repo: "baserepo".into(),
                packages: vec!["blah".into(), "bloo".into()],
            }])
        );
    }

    #[test]
    fn basic_valid_add_remove_files() {
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(indoc! {r#"
            add-files:
              - - foo
                - /usr/bin/foo
              - - baz
                - /usr/bin/blah
            remove-files:
             - foo
             - bar
        "#});
        let buf = buf.as_bytes();
        let mut input = io::BufReader::new(buf);
        let treefile =
            treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        assert!(treefile.base.add_files.unwrap().len() == 2);
        assert!(treefile.base.remove_files.unwrap().len() == 2);
    }

    #[test]
    fn basic_valid_rpmdb_target() {
        let tf = new_test_tf_basic(VALID_PRELUDE).unwrap();
        assert!(tf.rpmdb_backend_is_target());
        for v in &["target", "bdb"] {
            let mut buf = String::from(VALID_PRELUDE);
            buf.push_str(&format!("rpmdb: {}", v));
            let tf = new_test_tf_basic(buf.as_str()).unwrap();
            assert!(tf.rpmdb_backend_is_target());
        }
        {
            let mut buf = String::from(VALID_PRELUDE);
            buf.push_str("rpmdb: host");
            let tf = new_test_tf_basic(buf.as_str()).unwrap();
            assert!(!tf.rpmdb_backend_is_target());
        }
    }

    #[test]
    fn test_default() {
        let _cfg: TreeComposeConfig = Default::default();
    }

    #[test]
    fn basic_js_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE_JS.as_bytes());
        let mut treefile =
            treefile_parse_stream(utils::InputFormat::JSON, &mut input, Some(ARCH_X86_64)).unwrap();
        treefile = treefile.substitute_vars().unwrap();
        assert!(treefile.base.treeref.unwrap() == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 5);
    }

    #[test]
    fn basic_valid_noarch() {
        let mut input = io::BufReader::new(VALID_PRELUDE.as_bytes());
        let mut treefile =
            treefile_parse_stream(utils::InputFormat::YAML, &mut input, None).unwrap();
        treefile = treefile.substitute_vars().unwrap();
        assert!(treefile.base.treeref.unwrap() == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 5);
    }

    fn append_and_parse(append: &'static str) -> TreeComposeConfig {
        let buf = VALID_PRELUDE.to_string() + append;
        let mut input = io::BufReader::new(buf.as_bytes());
        let treefile =
            treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        treefile.substitute_vars().unwrap()
    }

    fn test_invalid(data: &'static str) {
        let buf = VALID_PRELUDE.to_string() + data;
        let mut input = io::BufReader::new(buf.as_bytes());
        match treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)) {
            Err(ref e) => match e.downcast_ref::<io::Error>() {
                Some(ref ioe) if ioe.kind() == io::ErrorKind::InvalidInput => {}
                _ => panic!("Expected invalid treefile, not {}", e.to_string()),
            },
            Ok(_) => panic!("Expected invalid treefile"),
        }
    }

    #[test]
    fn basic_valid_releasever() {
        let buf = indoc! {r#"
            ref: "exampleos/${basearch}/${releasever}"
            releasever: "30"
            automatic-version-prefix: ${releasever}
            mutate-os-release: ${releasever}
        "#};
        let mut input = io::BufReader::new(buf.as_bytes());
        let mut treefile =
            treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        treefile = treefile.substitute_vars().unwrap();
        assert!(treefile.base.treeref.unwrap() == "exampleos/x86_64/30");
        assert!(treefile.base.releasever.unwrap() == "30");
        assert!(treefile.base.automatic_version_prefix.unwrap() == "30");
        assert!(treefile.base.mutate_os_release.unwrap() == "30");
        assert!(treefile.base.rpmdb.is_none());
    }

    #[test]
    fn test_valid_no_releasever() {
        let treefile = append_and_parse("automatic_version_prefix: ${releasever}");
        assert!(treefile.base.releasever == None);
        assert!(treefile.base.automatic_version_prefix.unwrap() == "${releasever}");
    }

    #[test]
    fn basic_valid_legacy() {
        let treefile = append_and_parse(indoc! {"
            gpg_key: foo
            boot_location: new
            default_target: bar
            automatic_version_prefix: baz
            rpmdb: sqlite
        "});
        assert!(treefile.base.gpg_key.unwrap() == "foo");
        assert!(treefile.base.boot_location.unwrap() == BootLocation::New);
        assert!(treefile.base.default_target.unwrap() == "bar");
        assert!(treefile.base.automatic_version_prefix.unwrap() == "baz");
        assert!(treefile.base.rpmdb.unwrap() == RpmdbBackend::Sqlite);
    }

    #[test]
    fn basic_valid_legacy_new() {
        let treefile = append_and_parse(indoc! {"
            gpg-key: foo
            boot-location: new
            default-target: bar
            automatic-version-prefix: baz
            rpmdb: b-d-b
        "});
        assert!(treefile.base.gpg_key.unwrap() == "foo");
        assert!(treefile.base.boot_location.unwrap() == BootLocation::New);
        assert!(treefile.base.default_target.unwrap() == "bar");
        assert!(treefile.base.automatic_version_prefix.unwrap() == "baz");
        assert!(treefile.base.rpmdb.unwrap() == RpmdbBackend::Bdb);
    }

    #[test]
    fn basic_invalid_legacy_both() {
        test_invalid(indoc! {"
            gpg-key: foo
            gpg_key: bar
        "});
        test_invalid(indoc! {"
            boot-location: new
            boot_location: both
        "});
        test_invalid(indoc! {"
            default-target: foo
            default_target: bar
        "});
        test_invalid(indoc! {"
            automatic-version-prefix: foo
            automatic_version_prefix: bar
        "});
    }

    #[test]
    fn test_invalid_install_langs() {
        test_invalid(indoc! {r#"
            install_langs:
                - "klingon"
                - "esperanto"
        "#});
    }

    #[test]
    fn test_invalid_arch_packages_type() {
        test_invalid(indoc! {"
            packages-hal9000: true
        "});
    }

    #[test]
    fn test_invalid_arch_packages_array_type() {
        test_invalid(indoc! {"
            packages-hal9000:
                - 12
                - 34
        "});
    }

    pub(crate) fn new_test_treefile<'a, 'b>(
        workdir: &std::path::Path,
        contents: &'a str,
        basearch: Option<&'b str>,
    ) -> Result<Box<Treefile>> {
        let tf_path = workdir.join("treefile.yaml");
        std::fs::write(&tf_path, contents)?;
        Ok(Treefile::new_boxed(
            tf_path.as_path(),
            basearch,
            Some(openat::Dir::open(workdir)?),
        )?)
    }

    pub(crate) fn new_test_tf_basic(contents: impl AsRef<str>) -> Result<Box<Treefile>> {
        let contents = contents.as_ref();
        let workdir = tempfile::tempdir().unwrap();
        new_test_treefile(workdir.path(), contents, None)
    }

    #[test]
    fn test_treefile_new() {
        let workdir = tempfile::tempdir().unwrap();
        let tf = new_test_treefile(workdir.path(), VALID_PRELUDE, None).unwrap();
        assert!(tf.parsed.base.rojig.is_none());
        assert!(tf.parsed.base.machineid_compat.is_none());
    }

    const ROJIG_YAML: &'static str = indoc! {r#"
        rojig:
            name: "exampleos"
            license: "MIT"
            summary: "ExampleOS rojig base image"
    "#};

    // We need to support rojig: for a long time because it's used by fedora-coreos-config/coreos-assembler at least.
    #[test]
    fn test_treefile_new_rojig() {
        let workdir = tempfile::tempdir().unwrap();
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(ROJIG_YAML);
        let tf = new_test_treefile(workdir.path(), buf.as_str(), None).unwrap();
        let rojig = tf.parsed.base.rojig.as_ref().unwrap();
        assert!(rojig.name == "exampleos");
    }

    #[test]
    fn test_treefile_includes() -> Result<()> {
        let workdir = tempfile::tempdir()?;
        let workdir_d = openat::Dir::open(workdir.path())?;
        workdir_d.write_file_contents(
            "foo.yaml",
            0o644,
            indoc! {"
                repos:
                    - foo
                packages:
                    - fooinclude
                repo-packages:
                    # this entry is overridden by the last entry; so will disappear
                    - repo: foo
                      packages:
                        - qwert
                    # this entry is overridden by the prelude treefile; so will disappear
                    - repo: foob
                      packages:
                        - blah
                    - repo: foo2
                      packages:
                        - qwert
                modules:
                    enable:
                      - dodo
                    install:
                      - bazboo
            "},
        )?;
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(indoc! {"
            include: foo.yaml
        "});
        let tf = new_test_treefile(workdir.path(), buf.as_str(), None)?;
        assert!(tf.parsed.packages.unwrap().len() == 6);
        assert_eq!(
            tf.parsed.repo_packages,
            Some(vec![
                RepoPackage {
                    repo: "foo2".into(),
                    packages: vec!["qwert".into()],
                },
                RepoPackage {
                    repo: "baserepo".into(),
                    packages: vec!["blah".into(), "bloo".into()],
                }
            ])
        );
        assert_eq!(
            tf.parsed.modules,
            Some(ModulesConfig {
                enable: Some(vec!["dodo".into(), "foobar:2.0".into()]),
                install: Some(vec![
                    "bazboo".into(),
                    "nodejs:15".into(),
                    "swig:3.0/complete".into(),
                    "sway:rolling".into(),
                ])
            },)
        );
        Ok(())
    }

    #[test]
    fn test_treefile_arch_includes() -> Result<()> {
        let workdir = tempfile::tempdir()?;
        let workdir_d = openat::Dir::open(workdir.path())?;
        workdir_d.write_file_contents(
            "foo-x86_64.yaml",
            0o644,
            r#"
repos:
  - foo
packages:
  - foo-x86_64-include
"#,
        )?;
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(
            r#"
arch-include:
    x86_64: foo-x86_64.yaml
    s390x: foo-s390x.yaml
"#,
        );
        // Note foo-s390x.yaml doesn't exist
        let tf = new_test_treefile(workdir.path(), buf.as_str(), Some(ARCH_X86_64))?;
        assert!(tf
            .parsed
            .packages
            .unwrap()
            .iter()
            .find(|&p| p == "foo-x86_64-include")
            .is_some());
        Ok(())
    }

    #[test]
    fn test_treefile_merge() {
        let basearch = Some(ARCH_X86_64);
        let mut base = append_and_parse(indoc! {r#"
            add-commit-metadata:
                my-first-key: "please don't override me"
                my-second-key: "override me"
            etc-group-members:
                - sudo
        "#});
        let mut mid_input = io::BufReader::new(
            indoc! {r#"
            packages:
              - some layered packages
            add-commit-metadata:
              my-second-key: "something better"
              my-third-key: 1000
              my-fourth-key:
                nested: table
            etc-group-members:
              - docker
        "#}
            .as_bytes(),
        );
        let mut mid =
            treefile_parse_stream(utils::InputFormat::YAML, &mut mid_input, basearch).unwrap();
        let mut top_input = io::BufReader::new(ROJIG_YAML.as_bytes());
        let mut top =
            treefile_parse_stream(utils::InputFormat::YAML, &mut top_input, basearch).unwrap();
        assert!(top.base.add_commit_metadata.is_none());
        treefile_merge(&mut mid, &mut base);
        treefile_merge(&mut top, &mut mid);
        let tf = &top;
        assert!(tf.packages.as_ref().unwrap().len() == 10);
        assert!(tf.base.etc_group_members.as_ref().unwrap().len() == 2);
        let rojig = tf.base.rojig.as_ref().unwrap();
        assert!(rojig.name == "exampleos");
        let data = tf.base.add_commit_metadata.as_ref().unwrap();
        assert!(data.get("my-first-key").unwrap().as_str().unwrap() == "please don't override me");
        assert!(data.get("my-second-key").unwrap().as_str().unwrap() == "something better");
        assert!(data.get("my-third-key").unwrap().as_i64().unwrap() == 1000);
        assert!(
            data.get("my-fourth-key")
                .unwrap()
                .as_object()
                .unwrap()
                .get("nested")
                .unwrap()
                .as_str()
                .unwrap()
                == "table"
        );
    }

    #[test]
    fn test_split_whitespace_unless_quoted() -> Result<()> {
        // test single quoted package
        let single_quoted_pkg = "'foobar >= 1.0'";
        let pkgs: Vec<_> = split_whitespace_unless_quoted(&single_quoted_pkg)?.collect();
        assert_eq!("foobar >= 1.0", pkgs[0]);

        // test multiple quoted packages
        let mult_quoted_pkg = "'foobar >= 1.0' 'quuz < 0.5' 'corge > 2'";
        let pkgs: Vec<_> = split_whitespace_unless_quoted(&mult_quoted_pkg)?.collect();
        assert_eq!("foobar >= 1.0", pkgs[0]);
        assert_eq!("quuz < 0.5", pkgs[1]);
        assert_eq!("corge > 2", pkgs[2]);

        // test single unquoted package
        let single_unquoted_pkg = "foobar";
        let pkgs: Vec<_> = split_whitespace_unless_quoted(&single_unquoted_pkg)?.collect();
        assert_eq!("foobar", pkgs[0]);

        // test multiple unquoted packages
        let mult_unquoted_pkg = "foobar quuz corge";
        let pkgs: Vec<_> = split_whitespace_unless_quoted(&mult_unquoted_pkg)?.collect();
        assert_eq!("foobar", pkgs[0]);
        assert_eq!("quuz", pkgs[1]);
        assert_eq!("corge", pkgs[2]);

        // test different orderings of mixed quoted and unquoted packages
        let mix_quoted_unquoted_pkgs = "'foobar >= 1.1' baz-package 'corge < 0.5'";
        let pkgs: Vec<_> = split_whitespace_unless_quoted(&mix_quoted_unquoted_pkgs)?.collect();
        assert_eq!("foobar >= 1.1", pkgs[0]);
        assert_eq!("baz-package", pkgs[1]);
        assert_eq!("corge < 0.5", pkgs[2]);
        let mix_quoted_unquoted_pkgs = "corge 'foobar >= 1.1' baz-package";
        let pkgs: Vec<_> = split_whitespace_unless_quoted(&mix_quoted_unquoted_pkgs)?.collect();
        assert_eq!("corge", pkgs[0]);
        assert_eq!("foobar >= 1.1", pkgs[1]);
        assert_eq!("baz-package", pkgs[2]);
        let mix_quoted_unquoted_pkgs = "corge 'foobar >= 1.1' baz-package 'quuz < 0.0.1'";
        let pkgs: Vec<_> = split_whitespace_unless_quoted(&mix_quoted_unquoted_pkgs)?.collect();
        assert_eq!("corge", pkgs[0]);
        assert_eq!("foobar >= 1.1", pkgs[1]);
        assert_eq!("baz-package", pkgs[2]);
        assert_eq!("quuz < 0.0.1", pkgs[3]);

        // test missing quotes around packages using version qualifiers
        let missing_quotes = "foobar >= 1.0 quuz";
        let pkgs: Vec<_> = split_whitespace_unless_quoted(&missing_quotes)?.collect();
        assert_ne!("foobar >= 1.0", pkgs[0]);
        assert_eq!(">=", pkgs[1]);
        let missing_leading_quote = "foobar >= 1.0'";
        assert!(split_whitespace_unless_quoted(&missing_leading_quote).is_err());
        let missing_trailing_quote = "'foobar >= 1.0 baz-package";
        assert!(split_whitespace_unless_quoted(&missing_trailing_quote).is_err());
        let stray_quote = "'foobar >= 1.0' quuz' corge";
        assert!(split_whitespace_unless_quoted(&stray_quote).is_err());

        Ok(())
    }

    #[test]
    fn test_write_json() -> Result<()> {
        let rootdir = tempfile::tempdir()?;
        let rootdir = &openat::Dir::open(rootdir.path())?;
        {
            let workdir = tempfile::tempdir()?;
            let tf = new_test_treefile(workdir.path(), VALID_PRELUDE, None).unwrap();
            tf.write_compose_json(rootdir)?;
        }
        let mut src = std::io::BufReader::new(rootdir.open_file(COMPOSE_JSON_PATH)?);
        let cfg = treefile_parse_stream(utils::InputFormat::JSON, &mut src, None)?;
        assert_eq!(cfg.base.treeref.unwrap(), "exampleos/x86_64/blah");
        Ok(())
    }

    #[test]
    fn test_check_passwd() {
        {
            let workdir = tempfile::tempdir().unwrap();
            let tf = new_test_treefile(workdir.path(), VALID_PRELUDE, None).unwrap();
            let default_cfg = tf.parsed.get_check_passwd();
            assert_eq!(default_cfg, &CheckPasswd::Previous);
        }
        {
            let input = VALID_PRELUDE.to_string() + r#"check-passwd: { "type": "none" }"#;
            let workdir = tempfile::tempdir().unwrap();
            let tf = new_test_treefile(workdir.path(), &input, None).unwrap();
            let custom_cfg = tf.parsed.get_check_passwd();
            assert_eq!(custom_cfg, &CheckPasswd::None);
        }
        {
            let input = VALID_PRELUDE.to_string()
                + r#"check-passwd: { "type": "data", "entries": { "bin": 1, "adm": [3, 4], "foo" : [2] } }"#;
            let workdir = tempfile::tempdir().unwrap();
            let tf = new_test_treefile(workdir.path(), &input, None).unwrap();
            let custom_cfg = tf.parsed.get_check_passwd();
            let data = match custom_cfg {
                CheckPasswd::Data(ref v) => v,
                x => panic!("unexpected variant {:?}", x),
            };
            assert_eq!(
                data,
                &CheckPasswdData {
                    entries: maplit::btreemap!(
                        "adm".into() => (3, 4).into(),
                        "bin".into() => 1.into(),
                        "foo".into() => [2].into(),
                    ),
                }
            );
            let ids: Vec<_> = data.entries.iter().map(|(_k, v)| v.ids()).collect();
            let expected = vec![
                (Uid::from_raw(3), Gid::from_raw(4)),
                (Uid::from_raw(1), Gid::from_raw(1)),
                (Uid::from_raw(2), Gid::from_raw(2)),
            ];
            assert_eq!(ids, expected);
        }
        {
            let input = VALID_PRELUDE.to_string()
                + r#"check-passwd: { "type": "file", "filename": "local-file" }"#;
            let workdir = tempfile::tempdir().unwrap();
            let workdir_d = openat::Dir::open(workdir.path()).unwrap();
            workdir_d
                .write_file_contents("local-file", 0o755, "")
                .unwrap();
            let tf = new_test_treefile(workdir.path(), &input, None).unwrap();
            let custom_cfg = tf.parsed.get_check_passwd();
            assert_eq!(
                custom_cfg,
                &CheckPasswd::File(CheckFile {
                    filename: "local-file".to_string()
                })
            );
        }
    }

    #[test]
    fn test_check_groups() {
        {
            let workdir = tempfile::tempdir().unwrap();
            let tf = new_test_treefile(workdir.path(), VALID_PRELUDE, None).unwrap();
            let default_cfg = tf.parsed.get_check_groups();
            assert_eq!(default_cfg, &CheckGroups::Previous);
        }
        {
            let input = VALID_PRELUDE.to_string() + r#"check-groups: { "type": "none" }"#;
            let workdir = tempfile::tempdir().unwrap();
            let tf = new_test_treefile(workdir.path(), &input, None).unwrap();
            let custom_cfg = tf.parsed.get_check_groups();
            assert_eq!(custom_cfg, &CheckGroups::None);
        }
        {
            let input = VALID_PRELUDE.to_string()
                + r#"check-groups: { "type": "data", "entries": { "bin": 1 } }"#;
            let workdir = tempfile::tempdir().unwrap();
            let tf = new_test_treefile(workdir.path(), &input, None).unwrap();
            let custom_cfg = tf.parsed.get_check_groups();
            assert_eq!(
                custom_cfg,
                &CheckGroups::Data(CheckGroupsData {
                    entries: maplit::btreemap!(
                        "bin".into() => 1,
                    ),
                })
            );
        }
        {
            let input = VALID_PRELUDE.to_string()
                + r#"check-groups: { "type": "file", "filename": "local-file" }"#;
            let workdir = tempfile::tempdir().unwrap();
            let workdir_d = openat::Dir::open(workdir.path()).unwrap();
            workdir_d
                .write_file_contents("local-file", 0o755, "")
                .unwrap();
            let tf = new_test_treefile(workdir.path(), &input, None).unwrap();
            let custom_cfg = tf.parsed.get_check_groups();
            assert_eq!(
                custom_cfg,
                &CheckGroups::File(CheckFile {
                    filename: "local-file".to_string()
                })
            );
        }
    }
}

// Some of our file descriptors may be read multiple times.
// We try to consistently seek to the start to make that
// convenient from the C side.  Note that this function
// will abort if seek() fails (it really shouldn't).
fn raw_seeked_fd(fd: &mut std::fs::File) -> RawFd {
    fd.seek(std::io::SeekFrom::Start(0)).expect("seek");
    fd.as_raw_fd()
}

pub(crate) fn treefile_new(
    filename: &str,
    basearch: &str,
    workdir: i32,
) -> CxxResult<Box<Treefile>> {
    let basearch = opt_string(basearch);
    let workdir = if workdir != -1 {
        Some(crate::ffiutil::ffi_view_openat_dir(workdir))
    } else {
        None
    };
    Ok(Treefile::new_boxed(filename.as_ref(), basearch, workdir)?)
}

/// Create a new treefile from a string.
// Make `client` into an enum?
pub(crate) fn treefile_new_from_string(buf: &str, client: bool) -> CxxResult<Box<Treefile>> {
    let r = Treefile::new_from_string(buf).context("Parsing treefile from string")?;
    match client {
        true => r.error_if_base()?,
        false => r.error_if_deriving()?,
    }
    Ok(r)
}

/// Create a new empty treefile.
pub(crate) fn treefile_new_empty() -> CxxResult<Box<Treefile>> {
    Ok(Treefile::new_from_string("{}")?)
}

/// Create a new treefile, returning an error if any (currently) client-side options are set.
pub(crate) fn treefile_new_compose(
    filename: &str,
    basearch: &str,
    workdir: i32,
) -> CxxResult<Box<Treefile>> {
    let r = treefile_new(filename, basearch, workdir)?;
    Treefile::validate_base_config(&r.parsed)?;
    r.error_if_deriving()?;
    Ok(r)
}

/// Create a new treefile, returning an error if any (currently) compose-side options are set.
pub(crate) fn treefile_new_client(filename: &str, basearch: &str) -> CxxResult<Box<Treefile>> {
    let r = treefile_new(filename, basearch, -1)?;
    r.error_if_base()?;
    Ok(r)
}

fn iter_etc_treefiles() -> Result<impl Iterator<Item = Result<PathBuf>>> {
    Ok(read_dir(CLIENT_TREEFILES_DIR)?.filter_map(|res| match res {
        Ok(e) => {
            let path = e.path();
            if let Some(ext) = path.extension() {
                if ext == "yaml" {
                    return Some(anyhow::Result::Ok(path));
                }
            }
            None
        }
        Err(err) => Some(Err(anyhow::Error::msg(err))),
    }))
}

/// Create a new client treefile using the treefile dropins in /etc/rpm-ostree/origin.d/.
pub(crate) fn treefile_new_client_from_etc(basearch: &str) -> CxxResult<Box<Treefile>> {
    let basearch = opt_string(basearch);
    let mut cfg = TreeComposeConfig::default();
    let mut tfs = iter_etc_treefiles()?.collect::<Result<Vec<PathBuf>>>()?;
    tfs.sort(); // sort because order matters; later treefiles override earlier ones
    for tf in tfs {
        let new_cfg = treefile_parse_and_process(tf, basearch)?;
        new_cfg.config.base.error_if_nonempty()?;
        new_cfg.externals.assert_nonempty();
        let mut new_cfg = new_cfg.config;
        treefile_merge(&mut new_cfg, &mut cfg);
        cfg = new_cfg;
    }
    let r = Treefile::new_from_config(cfg, None)?;
    r.error_if_base()?;
    Ok(Box::new(r))
}

pub(crate) fn treefile_delete_client_etc() -> CxxResult<()> {
    // To be nice we don't delete the directory itself; just matching files.
    for tf in iter_etc_treefiles()? {
        std::fs::remove_file(&tf?)?;
    }
    Ok(())
}
