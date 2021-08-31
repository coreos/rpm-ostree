#!/bin/bash
set -euo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

set -x
make -f .copr/Makefile srpm
./packaging/rpmbuild-cwd --with bin-unit-tests --rebuild packaging/*.src.rpm
mv $(arch)/*.rpm .
