#!/usr/bin/bash
# We use https://cxx.rs to generate C++ and Rust bridge code.  If you change
# rust/src/lib.rs, you will need to install the tool.
set -xeuo pipefail
CXX_VER=$(cargo metadata --format-version 1 | jq -r '.packages[]|select(.name == "cxx").version')
mkdir -p target
time cargo install --root=target/cxxbridge cxxbridge-cmd --version "${CXX_VER}"
