//! Helpers for the client side binary that will speak DBus
//! to rpm-ostreed.service.

use crate::cxxrsutil::*;
use crate::utils;
use std::os::unix::io::IntoRawFd;

fn is_http(arg: &str) -> bool {
    arg.starts_with("https://") || arg.starts_with("http://")
}

/// Given a string from the command line, determine if it represents one or more
/// RPM URLs we need to fetch, and if so download those URLs and return file
/// descriptors for the content.
/// TODO(cxx-rs): This would be slightly more elegant as Result<Option<Vec<i32>>>
pub(crate) fn client_handle_fd_argument(arg: &str, arch: &str) -> CxxResult<Vec<i32>> {
    #[cfg(feature = "fedora-integration")]
    if let Some(fds) = crate::fedora_integration::handle_cli_arg(arg, arch)? {
        return Ok(fds.into_iter().map(|f| f.into_raw_fd()).collect());
    }

    if is_http(arg) {
        Ok(utils::download_url_to_tmpfile(arg, true).map(|f| vec![f.into_raw_fd()])?)
    } else if arg.ends_with(".rpm") {
        Ok(vec![std::fs::File::open(arg)?.into_raw_fd()])
    } else {
        Ok(Vec::new())
    }
}
