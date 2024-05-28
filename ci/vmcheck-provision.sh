#!/usr/bin/bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Install build dependencies, run unit tests and installed tests.

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh
pkg_install openssh-clients
