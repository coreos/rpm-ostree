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
rm -f clang-unformatted.txt
git ls-files '**.c' '**.cxx' '**.h' '**.hpp' | while read f; do
    if ! clang-format --Werror --ferror-limit=1 --dry-run "$f" > /dev/null 2>&1; then
        echo "$f" >> clang-unformatted.txt
    fi
done
if test -s clang-unformatted.txt; then
    echo "Error: clang-format needed on these files:" 1>&2
    cat clang-unformatted.txt 1>&2
    exit 1
fi
rm -f clang-unformatted.txt
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
