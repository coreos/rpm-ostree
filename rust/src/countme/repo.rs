/*
 * Copyright (C) 2020 Red Hat, Inc.
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

use anyhow::{Context, Result};
use std::fs;
use std::path::PathBuf;

use ini::Ini;

use crate::utils;

/// Location for DNF repositories configuration
pub const YUM_REPOS_D: &str = "/etc/yum.repos.d";

/// Repository configuration
/// Not exhaustive and only includes the options needed for Count Me support.
#[derive(Debug)]
pub struct Repo {
    name: String,
    enabled: bool,
    count_me: bool,
    meta_link: String,
}

/// From https://github.com/rpm-software-management/libdnf/blob/45981d5f53980dac362900df65bcb2652aa8d7c7/libdnf/conf/OptionBool.hpp#L30-L31
fn is_true(string: &str) -> bool {
    string == "1" || string == "yes" || string == "true" || string == "on"
}

/// Read all repository configuration files from the default location
pub fn all() -> Result<Vec<Repo>> {
    let configs = fs::read_dir(YUM_REPOS_D)
        .with_context(|| format!("Could not list files in: {}", YUM_REPOS_D))?;
    let mut repos = Vec::new();
    for c in configs {
        let path = c?.path();
        match parse_repo_file(&path) {
            Err(e) => {
                eprintln!(
                    "Failed to parse repo config file '{}': {}",
                    path.display(),
                    e
                )
            }
            Ok(mut r) => repos.append(&mut r),
        }
    }
    Ok(repos)
}

/// Read repository configuration from a file
fn parse_repo_file(path: &PathBuf) -> Result<Vec<Repo>> {
    let i = Ini::load_from_file(path).with_context(|| "Could not parse as INI".to_string())?;
    let mut repos = Vec::new();
    for (sec, prop) in i.iter() {
        let mut repo = match sec {
            None => {
                continue;
            }
            Some(s) => Repo {
                name: String::from(s),
                enabled: false,
                count_me: false,
                meta_link: "".to_string(),
            },
        };
        for (k, v) in prop.iter() {
            match k {
                "countme" => {
                    if is_true(v) {
                        repo.count_me = true
                    }
                }
                "enabled" => {
                    if is_true(v) {
                        repo.enabled = true
                    }
                }
                "metalink" => repo.meta_link = String::from(v),
                _ => {}
            }
        }
        repos.push(repo);
    }
    Ok(repos)
}

impl Repo {
    /// Returns true if this repo is
    /// - enabled
    /// - configured for sending a Count Me request
    /// - has a non-empty metalink URL
    pub fn count_me(&self) -> bool {
        self.enabled && self.count_me && !self.meta_link.is_empty()
    }

    /// Get the metalink URL for the repo with variables replaced
    pub fn metalink(&self, version_id: &str) -> String {
        self.meta_link
            .clone()
            .replace("$releasever", &version_id)
            .replace("$basearch", &utils::get_rpm_basearch())
    }
}
