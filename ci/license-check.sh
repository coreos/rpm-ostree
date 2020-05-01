#!/bin/bash
set -xeuo pipefail
cargo install cargo-lichking
cargo lichking list
cargo lichking check
echo "License check OK"
