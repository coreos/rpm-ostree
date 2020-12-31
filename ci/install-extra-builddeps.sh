#!/usr/bin/bash
# cxx.rs (cxxbridge) isn't packaged in Fedora today.  Both it and cbindgen generate
# source code, which we vendor along with our dependent crates into release
# tarballs.  Note in the future it's likely we stop using cbindgen entirely in
# favor of cxx.rs.
set -xeuo pipefail
if ! command -v cxxbridge; then
    ver=$(cargo metadata --format-version 1 | jq -r '.packages[]|select(.name == "cxx").version')
    cargo install cxxbridge-cmd --version "${ver}"
fi
if ! command -v cbindgen; then
    ver=$(cargo metadata --format-version 1 | jq -r '.packages[]|select(.name == "cbindgen").version')
    cargo install cbindgen --version "${ver}"
fi
