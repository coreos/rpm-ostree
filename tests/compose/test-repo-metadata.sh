#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

treefile_set repo-metadata '"detached"'
runcompose
echo "ok compose detached"

ostree --repo=${repo} show --print-metadata-key rpmostree.rpmmd-repos ${treeref} && \
    fatal "rpmostree.rpmmd-repos present in inline metadata when should be detached"
ostree --repo=${repo} show --print-detached-metadata-key rpmostree.rpmmd-repos ${treeref} > meta.txt
assert_file_has_content meta.txt 'id.*cache.*timestamp'
echo "ok metadata detached"

treefile_set repo-metadata '"disabled"'
runcompose
echo "ok compose disabled"

ostree --repo=${repo} show --print-metadata-key rpmostree.rpmmd-repos ${treeref} && \
    fatal "rpmostree.rpmmd-repos present in inline metadata when should be disabled"
ostree --repo=${repo} show --print-detached-metadata-key rpmostree.rpmmd-repos ${treeref} && \
    fatal "rpmostree.rpmmd-repos present in detached metadata when should be disabled"
echo "ok metadata disabled"
