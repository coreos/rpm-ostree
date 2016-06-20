#!/bin/bash
set -euo pipefail

# ugly but simple way of fetching commit we're sitting on
commit=$(rpm-ostree status --json | \
  python -c '
import sys, json;
print json.load(sys.stdin)["deployments"][0]["checksum"]')

if [[ -z $commit ]] || ! ostree rev-parse $commit; then
  echo "Error while determining current commit" >&2
  exit 1
fi

cd /ostree/repo/tmp
umount vmcheck.ro 2>/dev/null ||:
rm -rf vmcheck*
ostree checkout $commit vmcheck --fsync=0
mkdir vmcheck.ro
rofiles-fuse vmcheck{,.ro}
