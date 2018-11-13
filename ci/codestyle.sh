#!/bin/sh
set -xeuo pipefail

echo "Checking for tabs:"
(git grep -E '^	+' -- '*.[ch]' || true) > tabdamage.txt
if test -s tabdamage.txt; then
    echo "Error: tabs in .[ch] files:"
    cat tabdamage.txt
    exit 1
fi

echo "ok"
