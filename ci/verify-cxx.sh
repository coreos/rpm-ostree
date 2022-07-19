#!/usr/bin/bash
# Verify that the cxx-generated C++ code is in sync
set -xeuo pipefail
dn=$(dirname $0)
$dn/install-cxx.sh
make -f Makefile.bindings bindings
if ! git diff; then
    echo "Found diff in cxx-generated code; please run: make -f Makefile.bindings bindings" 1>&2
    exit 1
fi
echo "ok: cxx generated code matches"
