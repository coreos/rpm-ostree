#!/bin/bash
# Test rpm-ostree compose tree --ex-rojig-output-rpm

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh
. ${dn}/../common/libtest.sh

prepare_compose_test "compose2jigdo"
if rpm-ostree --version | grep -q rust; then
    pysetjsonmember "rojig" '{ "name": "fedora-atomic-host", "license": "MIT", "summary": "Fedora Atomic Host"}'
    python <<EOF
import json, yaml
jd=json.load(open("$treefile"))
with open("$treefile.yaml", "w") as f:
  f.write(yaml.dump(jd))
EOF
    export treefile=$treefile.yaml
else
    pysetjsonmember "ex-rojig-spec" '"fedora-atomic-host-oirpm.spec"'
fi

mkdir cache
mkdir jigdo-output
runcompose --ex-rojig-output-rpm $(pwd)/jigdo-output --cachedir $(pwd)/cache --add-metadata-string version=42.0
rev=$(ostree --repo=repo-build rev-parse ${treeref})
find jigdo-output -name '*.rpm' | tee rpms.txt
assert_file_has_content rpms.txt 'fedora-atomic-host-42.0.*x86_64'
grep 'fedora-atomic-host.*x86_64\.rpm' rpms.txt | while read p; do
    rpm -qp --provides ${p} >>provides.txt
done
assert_file_has_content_literal provides.txt "rpmostree-jigdo-commit(${rev})"
echo "ok compose2jigdoRPM"

runcompose --force-nocache --ex-rojig-output-set $(pwd)/jigdo-output --cachedir $(pwd)/cache --add-metadata-string version=42.1
rev=$(ostree --repo=repo-build rev-parse ${treeref})
find jigdo-output -name '*.rpm' | tee rpms.txt
assert_file_has_content rpms.txt 'systemd.*x86_64'
assert_file_has_content rpms.txt 'ostree.*x86_64'
assert_file_has_content rpms.txt 'fedora-atomic-host-42.1.*x86_64'
echo "ok compose2jigdoSet"

