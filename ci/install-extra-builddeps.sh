#!/usr/bin/bash
# Install build dependencies not in the cosa buildroot already
set -xeuo pipefail
if ! command -v cxxbridge; then
    cargo install --root=/usr cxxbridge-cmd
fi
