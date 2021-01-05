use crate::utils;
use anyhow::Result;
use std::os::unix::io::IntoRawFd;

fn is_http(arg: &str) -> bool {
    arg.starts_with("https://") || arg.starts_with("http://")
}

/// Given a string from the command line, determine if it represents one or more
/// RPM URLs we need to fetch, and if so download those URLs and return file
/// descriptors for the content.
/// TODO(cxx-rs): This would be slightly more elegant as Result<Option<Vec<i32>>>
pub(crate) fn client_handle_fd_argument(arg: &str, _arch: &str) -> Result<Vec<i32>> {
    if is_http(arg) {
        utils::download_url_to_tmpfile(arg, true).map(|f| vec![f.into_raw_fd()])
    } else if arg.ends_with(".rpm") {
        Ok(vec![std::fs::File::open(arg)?.into_raw_fd()])
    } else {
        Ok(Vec::new())
    }
}
