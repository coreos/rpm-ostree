#!/bin/bash
set -euo pipefail

# ugly but simple way of fetching commit we're sitting on
commit=$(rpm-ostree status --json | \
  python -c '
import sys, json;
deployments = json.load(sys.stdin)["deployments"]
for deployment in deployments:
  if deployment["booted"]:
    print deployment["checksum"]
    exit()')

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
