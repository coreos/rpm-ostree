#!/bin/bash
set -euo pipefail

ci/installdeps.sh
ci/install-extra-builddeps.sh
export PATH="$HOME/.cargo/bin:$PATH"
ci/build.sh
