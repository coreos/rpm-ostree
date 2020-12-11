#!/usr/bin/bash
# Install build dependencies not in the cosa buildroot already
set -xeuo pipefail
if ! command -v cxxbridge; then
    ver=$(cargo metadata --format-version 1 | jq -r '.packages[]|select(.name == "cxx").version')
    cargo install --root=/usr cxxbridge-cmd --version "${ver}"
fi
