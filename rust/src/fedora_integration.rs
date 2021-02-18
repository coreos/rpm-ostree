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

lazy_static::lazy_static! {
    /// See https://github.com/cgwalters/koji-sane-json-api
    static ref KOJI_JSON_API_HOST: String = {
        std::env::var("RPMOSTREE_KOJI_JSON_API_HOST").ok().unwrap_or_else(|| "kojiproxy-coreos.svc.ci.openshift.org".to_string())
    };
}

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
            let rpms = koji::get_rpm_urls_from_build(&buildid.nvr, arch, true)?;
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
    use std::collections::BTreeMap;

    #[derive(Default, Deserialize)]
    #[serde(rename_all = "kebab-case")]
    #[allow(dead_code)]
    pub(crate) struct KojiBuildInfo {
        nvr: String,
        id: u64,
        kojipkgs_url_prefix: String,
        rpms: BTreeMap<String, Vec<String>>,
    }

    impl KojiBuildInfo {
        fn rpmurl(&self, arch: &str, rpm: &str) -> String {
            format!("{}/{}/{}", self.kojipkgs_url_prefix, arch, rpm)
        }
    }

    pub(crate) fn get_buildid_from_url(url: &str) -> Result<&str> {
        let id = url.rsplit('?').next().expect("split");
        match id.strip_prefix("buildID=") {
            Some(s) => Ok(s),
            None => anyhow::bail!("Failed to parse Koji buildid from URL {}", url),
        }
    }

    pub(crate) fn get_build(buildid: &str) -> Result<KojiBuildInfo> {
        let url = format!("https://{}/buildinfo/{}", &*KOJI_JSON_API_HOST, buildid);
        let f = crate::utils::download_url_to_tmpfile(&url, false)
            .context("Failed to download buildinfo from koji proxy")?;
        Ok(serde_json::from_reader(std::io::BufReader::new(f))?)
    }

    pub(crate) fn get_rpm_urls_from_build(
        buildid: &str,
        arch: &str,
        skip_debug: bool,
    ) -> Result<impl IntoIterator<Item = String>> {
        let build = get_build(buildid)?;
        let mut ret = Vec::new();
        if let Some(rpms) = build.rpms.get(arch) {
            ret.extend(
                rpms.iter()
                    .filter(|r| !(skip_debug && is_debug_rpm(r)))
                    .map(|r| build.rpmurl(arch, r)),
            );
        }
        if let Some(rpms) = build.rpms.get("noarch") {
            ret.extend(rpms.iter().map(|r| build.rpmurl("noarch", r)));
        }
        Ok(ret.into_iter())
    }

    #[cfg(test)]
    mod test {
        use super::*;

        #[test]
        fn test_url_buildid() -> Result<()> {
            assert_eq!(
                get_buildid_from_url(
                    "https://koji.fedoraproject.org/koji/buildinfo?buildID=1637715"
                )?,
                "1637715"
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
        let urls: Vec<String> = koji::get_rpm_urls_from_build(&buildid, arch, true)?
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
