#!/bin/bash
set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "install-langs"
pysetjsonmember "install-langs" '["fr", "fr_FR", "en_US"]'
pysetjsonmember "postprocess-script" \"$PWD/lang-test.sh\"
cat > lang-test.sh << EOF
#!/bin/bash
set -xeuo pipefail
env LANG=fr_FR.UTF-8 date -ud @0 &> /etc/lang-test.date.txt
(env LANG=fr_FR.UTF-8 touch || :) &> /etc/lang-test.touch.txt
EOF
chmod a+x lang-test.sh
runcompose
echo "ok compose"

ostree --repo=${repobuild} cat ${treeref} /usr/etc/lang-test.date.txt > out.txt
assert_file_has_content out.txt 'jeu\. janv\.  1 00:00:00 UTC 1970'
ostree --repo=${repobuild} cat ${treeref} /usr/etc/lang-test.touch.txt > out.txt
assert_file_has_content out.txt 'opérande de fichier manquant'
echo "ok install-langs"
