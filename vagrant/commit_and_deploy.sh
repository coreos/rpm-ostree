#!/bin/bash
set -euo pipefail

cd /ostree/repo/tmp
umount vmcheck.ro

cmd="ostree commit -b vmcheck -s '' \
       --tree=dir=vmcheck \
       --link-checkout-speedup"

if [ -n "${VERSION:-}" ]; then
  cmd="$cmd --add-metadata-string=version=$VERSION"
fi

eval $cmd
ostree admin deploy vmcheck
sync
