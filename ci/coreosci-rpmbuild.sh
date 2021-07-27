#!/bin/bash
set -euo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

make -f .copr/Makefile srpm
./packaging/rpmbuild-cwd --rebuild packaging/*.src.rpm
mv $(arch)/*.rpm .
