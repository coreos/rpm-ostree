#!/usr/bin/env bash
# Tests that validate structure of the source code;
# can be run without building it.
set -euo pipefail

echo -n "checking for tabs... "
(git grep -E '^	+' -- '*.[ch]' || true) > tabdamage.txt
if test -s tabdamage.txt; then
    echo "Error: tabs in .[ch] files:" 1>&2
    cat tabdamage.txt 1>&2
    exit 1
fi
rm tabdamage.txt
echo "ok"

echo -n "checking clang-format... "
git ls-files '**.c' '**.cxx' '**.h' '**.hpp' | xargs clang-format --Werror --dry-run
echo "ok"

echo -n "checking rustfmt... "
for crate in $(find -iname Cargo.toml); do
    if ! cargo fmt --manifest-path ${crate} -- --check; then
        echo "cargo fmt failed; run: cd $(dirname ${crate}) && cargo fmt" 1>&2
        exit 1
    fi
done
echo "ok"

ident='SPDX-License-Identifier:'
echo -n "checking \"$ident\"... "
git ls-files '*.rs' | while read f; do
    if ! grep -qF "$ident" $f; then
        echo "error: File $f: Missing $ident" 1>&2
        exit 1
    fi
done
echo "ok"
