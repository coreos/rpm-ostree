#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

jq . "$treefile"
jq '{"repos", "releasever"}' < "$treefile" > manifest.json.new
cat >container.json << 'EOF'
{
  "packages": ["coreutils", "rpm"],
  "container": true,
  "selinux": false
}
EOF
cat manifest.json.new container.json | jq -s add > "$treefile"
jq . $treefile
runcompose
echo "ok compose container"
