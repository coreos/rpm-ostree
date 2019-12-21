#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

treefile_set "install-langs" '["fr", "fr_FR", "en_US"]'
treefile_set "postprocess-script" "'$PWD/lang-test.sh'"
cat > lang-test.sh << EOF
#!/bin/bash
set -xeuo pipefail
env LANG=fr_FR.UTF-8 date -ud @0 &> /etc/lang-test.date.txt
(env LANG=fr_FR.UTF-8 touch || :) &> /etc/lang-test.touch.txt
env LANG=de_DE.UTF-8 date -ud @0 &> /etc/lang-test.de.date.txt
(env LANG=de_DE.UTF-8 touch || :) &> /etc/lang-test.de.touch.txt
EOF
chmod a+x lang-test.sh
runcompose
echo "ok compose"

ostree --repo=${repo} cat ${treeref} /usr/etc/lang-test.date.txt > out.txt
assert_file_has_content out.txt 'jeu\. janv\.  1 00:00:00 UTC 1970'
ostree --repo=${repo} cat ${treeref} /usr/etc/lang-test.touch.txt > out.txt
assert_file_has_content out.txt 'opérande de fichier manquant'

# XXX: this test doesn't currently work since glibc no longer obeys install-langs:
# https://github.com/coreos/fedora-coreos-config/issues/194#issuecomment-556365516

# # check that de_DE was culled
# ostree --repo=${repo} cat ${treeref} /usr/etc/lang-test.de.date.txt > out.txt
# assert_file_has_content out.txt 'Thu Jan  1 00:00:00 UTC 1970'
# ostree --repo=${repo} cat ${treeref} /usr/etc/lang-test.de.touch.txt > out.txt
# assert_file_has_content out.txt 'missing file operand'

if ostree --repo=${repo} ls ${treeref} /usr/bin/rpmostree-postprocess-lang-test.sh 2>err.txt; then
    assert_not_reached "we failed to unlink?"
fi
assert_file_has_content err.txt "error: No such file or directory"

echo "ok install-langs"
