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

extern crate gio_sys;
extern crate glib;
extern crate glib_sys;
extern crate libc;

#[macro_use]
extern crate serde_derive;
extern crate serde;
extern crate serde_json;
extern crate serde_yaml;

use std::ffi::{CStr, OsStr};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::FromRawFd;
use std::path::Path;
use std::{fs, io};

mod glibutils;
use glibutils::*;

#[no_mangle]
pub extern "C" fn treefile_read(
    filename: *const libc::c_char,
    fd: libc::c_int,
    error: *mut *mut glib_sys::GError,
) -> libc::c_int {
    // using an O_TMPFILE is an easy way to avoid ownership transfer issues w/ returning allocated
    // memory across the Rust/C boundary

    // let's just get the unsafory stuff (see what I did there?) out of the way first
    let output_file = unsafe { fs::File::from_raw_fd(libc::dup(fd)) };
    let c_str: &CStr = unsafe { CStr::from_ptr(filename) };
    let filename_path = Path::new(OsStr::from_bytes(c_str.to_bytes()));

    treefile_read_impl(filename_path, output_file).to_glib_convention(error)
}

fn treefile_read_impl(filename: &Path, output: fs::File) -> io::Result<()> {
    let f = io::BufReader::new(fs::File::open(filename)?);

    let mut treefile: TreeComposeConfig = match serde_yaml::from_reader(f) {
        Ok(t) => t,
        Err(e) => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("{}", e),
            ))
        }
    };

    // special handling for packages, since we allow whitespaces within items
    if let Some(pkgs) = treefile.packages {
        treefile.packages = Some(whitespace_split_packages(&pkgs));
    }

    serde_json::to_writer_pretty(output, &treefile)?;

    Ok(())
}

fn whitespace_split_packages(pkgs: &[String]) -> Vec<String> {
    pkgs.iter().flat_map(|pkg| pkg.split_whitespace().map(String::from)).collect()
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
pub struct TreeComposeConfig {
    // Compose controls
    #[serde(rename = "ref")]
    pub treeref: String,
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
    #[serde(skip_serializing_if = "Option::is_none")]
    pub bootstrap_packages: Option<Vec<String>>,

    // Content installation opts
    #[serde(skip_serializing_if = "Option::is_none")]
    pub documentation: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub install_langs: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "initramfs-args")]
    pub initramfs_args: Option<Vec<String>>,

    // Tree layout options
    #[serde(default)]
    pub boot_location: BootLocation,
    #[serde(default)]
    pub tmp_is_dir: bool,

    // systemd
    #[serde(skip_serializing_if = "Option::is_none")]
    pub units: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub default_target: Option<String>,

    // versioning
    #[serde(skip_serializing_if = "Option::is_none")]
    pub releasever: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub automatic_version_prefix: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "mutate-os-relase")]
    pub mutate_os_release: Option<String>,

    // passwd-related bits
    #[serde(skip_serializing_if = "Option::is_none")]
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

    // Content manimulation
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "postprocess-script")]
    pub postprocess_script: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "add-files")]
    pub add_files: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub remove_files: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "remove-from-packages")]
    pub remove_from_packages: Option<Vec<Vec<String>>>,
}
