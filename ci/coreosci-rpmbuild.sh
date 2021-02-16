#!/bin/bash
set -euo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

# fetch tags so `git describe` gives a nice NEVRA when building the RPM
git fetch origin --tags
git submodule update --init --recursive
ci/installdeps.sh
# Our primary CI build goes via RPM rather than direct to binaries
# to better test that path, including our vendored spec file, etc.
# The RPM build expects pre-generated bindings, so do that now.
make -f Makefile.bindings bindings
cd packaging
make -f Makefile.dist-packaging rpm
