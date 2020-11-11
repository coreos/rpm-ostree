#!/usr/bin/env bash
set -euo pipefail

echo -n "Checking for tabs..."
(git grep -E '^	+' -- '*.[ch]' || true) > tabdamage.txt
if test -s tabdamage.txt; then
    echo "Error: tabs in .[ch] files:"
    cat tabdamage.txt
    exit 1
fi
echo "ok"

echo -n "checking rustfmt..."
cd rust
for crate in $(find -iname Cargo.toml); do
    if ! cargo fmt --manifest-path ${crate} -- --check; then
        echo "cargo fmt failed; run: cd $(dirname ${crate}) && cargo fmt" 1>&2
        exit 1
    fi
done
cd ..
echo "ok"
