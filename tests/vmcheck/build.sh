#!/bin/bash
set -xeuo pipefail
atomic run rpm-ostree-builder make
docker run --rm --privileged -ti -v /
