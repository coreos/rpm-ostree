#!/bin/bash
set -euo pipefail

cd /ostree/repo/tmp
umount vmcheck.ro

# would be nice to use --add-metadata-string=version=$(git describe) or so here,
# but we don't even have git!

ostree commit -b vmcheck -s '' --tree=dir=vmcheck --link-checkout-speedup
ostree admin deploy vmcheck
