//! APIs used to talk to Fedora Infrastructure tooling (Koji, Bodhi).
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::CxxResult;
use anyhow::{Context, Result};
use serde_derive::Deserialize;
use std::borrow::Cow;
use std::fs::File;

const KOJI_URL_PREFIX: &str = "https://koji.fedoraproject.org/koji/";
const BODHI_URL_PREFIX: &str = "https://bodhi.fedoraproject.org/updates/";
const BODHI_UPDATE_PREFIX: &str = "FEDORA-";

mod bodhi {
    use super::*;

    #[derive(Deserialize)]
    pub(crate) struct BodhiKojiBuild {
        pub(crate) nvr: String,
        /// Not currently included in koji URLs, so we ignore it
        #[allow(dead_code)]
        pub(crate) epoch: u64,
    }

    #[derive(Deserialize)]
    pub(crate) struct BodhiUpdate {
        pub(crate) builds: Vec<BodhiKojiBuild>,
    }

    #[derive(Deserialize)]
    struct BodhiUpdateResponse {
        update: BodhiUpdate,
    }

    pub(crate) fn canonicalize_update_id(id: &str) -> Cow<str> {
        let id = match id.strip_prefix(BODHI_URL_PREFIX) {
            Some(s) => return Cow::Borrowed(s),
            None => id,
        };
        match id.strip_prefix(BODHI_UPDATE_PREFIX) {
            Some(_) => Cow::Borrowed(id),
            None => Cow::Owned(format!("{}{}", BODHI_UPDATE_PREFIX, id)),
        }
    }

    pub(crate) fn get_update(updateid: &str) -> Result<BodhiUpdate> {
        let updateid = canonicalize_update_id(updateid);
        let url = format!("{}{}", BODHI_URL_PREFIX, updateid);
        let update_data = crate::utils::download_url_to_tmpfile(&url, false)
            .context("Failed to download bodhi update info")?;
        let resp: BodhiUpdateResponse = serde_json::from_reader(update_data)?;
        Ok(resp.update)
    }

    pub(crate) fn get_rpm_urls_from_update(updateid: &str, arch: &str) -> Result<Vec<String>> {
        let update = bodhi::get_update(updateid)?;
        update.builds.iter().try_fold(Vec::new(), |mut r, buildid| {
            // For now hardcode skipping debuginfo because it's large and hopefully
            // people aren't layering that.
            let buildid = koji::BuildReference::Nvr(buildid.nvr.to_string());
            let rpms = koji::get_rpm_urls_from_build(&buildid, arch, true)?;
            r.extend(rpms);
            Ok(r)
        })
    }

    #[cfg(test)]
    mod test {
        use super::*;

        #[test]
        fn test_canonicalize() {
            let regid = "FEDORA-2020-053d8a2e94";
            let shortid = "2020-053d8a2e94";
            let url = "https://bodhi.fedoraproject.org/updates/FEDORA-2020-053d8a2e94";
            assert_eq!(canonicalize_update_id(regid), regid);
            assert_eq!(canonicalize_update_id(url), regid);
            assert_eq!(canonicalize_update_id(shortid), regid);
        }
    }
}

mod koji {
    use super::*;
    use anyhow::anyhow;
    use std::collections::BTreeMap;
    use xmlrpc::{Request, Value};

    const KOJI_HUB: &str = "https://koji.fedoraproject.org/kojihub/";
    const TOPURL: &str = "https://kojipkgs.fedoraproject.org/";

    pub(crate) fn get_buildid_from_url(url: &str) -> Result<i64> {
        let id = url.rsplit('?').next().expect("split");
        match id.strip_prefix("buildID=") {
            Some(s) => Ok(s.parse()?),
            None => anyhow::bail!("Failed to parse Koji buildid from URL {}", url),
        }
    }

    fn xmlrpc_require_str<'a>(
        val: &'a BTreeMap<String, Value>,
        k: impl AsRef<str>,
    ) -> Result<&'a str> {
        let k = k.as_ref();
        let s = val.get(k).ok_or_else(|| anyhow!("Missing key {}", k))?;
        Ok(s.as_str()
            .ok_or_else(|| anyhow!("Key {} is not a string", k))?)
    }

    pub(crate) fn rpm_path_from_koji_rpm(
        pkgname: &str,
        kojirpm: &BTreeMap<String, Value>,
    ) -> Result<String> {
        let arch = xmlrpc_require_str(kojirpm, "arch")?;
        let version = xmlrpc_require_str(kojirpm, "version")?;
        let release = xmlrpc_require_str(kojirpm, "release")?;
        let nvr = xmlrpc_require_str(kojirpm, "nvr")?;

        Ok(format!(
            "packages/{}/{}/{}/{}/{}.{}.rpm",
            pkgname, version, release, arch, nvr, arch
        ))
    }

    pub(crate) enum BuildReference {
        Id(i64),
        Nvr(String),
    }

    pub(crate) fn get_rpm_urls_from_build(
        buildid: &BuildReference,
        target_arch: &str,
        skip_debug: bool,
    ) -> Result<impl IntoIterator<Item = String>> {
        let req = match buildid {
            BuildReference::Id(id) => Request::new("getBuild").arg(*id),
            BuildReference::Nvr(nvr) => Request::new("getBuild").arg(nvr.as_str()),
        };
        let res = req.call_url(KOJI_HUB).context("Invoking koji getBuild()")?;
        let res = res
            .as_struct()
            .ok_or_else(|| anyhow!("Expected struct from getBuild"))?;
        let package_name = xmlrpc_require_str(res, "name")?;
        let buildid = res
            .get("id")
            .ok_or_else(|| anyhow!("Missing `id` in getBuild"))?;
        let buildid = buildid
            .as_i64()
            .ok_or_else(|| anyhow!("getBuild id is not an i64"))?;

        let req = Request::new("listRPMs").arg(buildid);
        let arches = &[target_arch, "noarch"];
        let mut ret = Vec::new();
        for build in req
            .call_url(KOJI_HUB)
            .context("Invoking koji listRPMs")?
            .as_array()
            .ok_or_else(|| anyhow!("Expected array from listRPMs"))?
        {
            let build = build
                .as_struct()
                .ok_or_else(|| anyhow!("Expected struct in listRPMs"))?;
            let arch = xmlrpc_require_str(build, "arch")?;
            if !arches.contains(&arch) {
                continue;
            }
            let name = xmlrpc_require_str(build, "name")?;

            if skip_debug && is_debug_rpm(name) {
                continue;
            }

            let mut path = rpm_path_from_koji_rpm(package_name, build)?;
            path.insert_str(0, TOPURL);
            ret.push(path);
        }
        Ok(ret.into_iter())
    }

    #[cfg(test)]
    mod test {
        use super::*;

        #[ignore]
        #[test]
        fn test_get_build() -> Result<()> {
            let buildid = BuildReference::Id(1746721);

            get_rpm_urls_from_build(&buildid, "x86_64", false)?;
            Ok(())
        }

        #[test]
        fn test_url_buildid() -> Result<()> {
            assert_eq!(
                get_buildid_from_url(
                    "https://koji.fedoraproject.org/koji/buildinfo?buildID=1637715"
                )?,
                1637715
            );
            Ok(())
        }
    }
}

fn is_debug_rpm(rpm: &str) -> bool {
    rpm.contains("-debuginfo-") || rpm.contains("-debugsource-")
}

pub(crate) fn handle_cli_arg(url: &str, arch: &str) -> CxxResult<Option<Vec<File>>> {
    if url.starts_with(BODHI_URL_PREFIX) {
        let urls = bodhi::get_rpm_urls_from_update(url, arch)?;
        Ok(Some(
            crate::utils::download_urls_to_tmpfiles(urls, true)
                .context("Failed to download RPMs")?,
        ))
    } else if url.starts_with(KOJI_URL_PREFIX) {
        let buildid = koji::get_buildid_from_url(url)?;
        let urls: Vec<String> =
            koji::get_rpm_urls_from_build(&koji::BuildReference::Id(buildid), arch, true)?
                .into_iter()
                .collect();
        Ok(Some(
            crate::utils::download_urls_to_tmpfiles(urls, true)
                .context("Failed to download RPMs")?,
        ))
    } else {
        Ok(None)
    }
}
