#!/bin/bash
# Today, this runs as part of the coreos-assembler container with
# the latest rpm-ostree binary installed.  In the future it
# should derive from an "ostree native container"; see 
# https://github.com/ostreedev/ostree-rs-ext/#module-container-bridging-between-ostree-and-ocidocker-images
set -xeuo pipefail

fatal ()
{
  echo "$0: fatal: $*" >&2
  exit 1
}

# For now, we're just testing that `rpm-ostree install foo` will fail.

if rpm-ostree install cowsay 2>err.txt; then
    fatal "install worked unexpectedly"
fi
if ! grep -qF 'This system was not booted via libostree' err.txt; then
    cat err.txt
    fatal "Should have found container error"
fi
echo "ok install in container"
