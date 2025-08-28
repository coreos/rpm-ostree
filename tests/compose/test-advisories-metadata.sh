#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

treefile_set advisories-metadata '"detached"'
runcompose
echo "ok compose detached"


ostree --repo=${repo} show --print-metadata-key rpmostree.advisories ${treeref} && \
    fatal "rpmostree.advisories present in inline metadata when should be detached"
ostree --repo=${repo} show --print-detached-metadata-key rpmostree.advisories ${treeref} && \
    echo "ok metadata detached" || echo "rpmostree.advisories missing (no advisories in repo ?)"

treefile_set advisories-metadata '"disabled"'
runcompose
echo "ok compose disabled"

ostree --repo=${repo} show --print-metadata-key rpmostree.advisories ${treeref} && \
    fatal "rpmostree.advisories present in inline metadata when should be disabled"
ostree --repo=${repo} show --print-detached-metadata-key rpmostree.advisories ${treeref} && \
    fatal "rpmostree.advisories present in detached metadata when should be disabled"
echo "ok metadata disabled"
