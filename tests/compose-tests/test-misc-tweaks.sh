#!/bin/bash

set -xeuo pipefail

dn=$(cd $(dirname $0) && pwd)
. ${dn}/libcomposetest.sh

prepare_compose_test "misc-tweaks"
# No docs
pysetjsonmember "documentation" "False"
# Note this overrides:
# $ rpm -q systemd
# systemd-238-8.git0e0aa59.fc28.x86_64
# $ rpm -qlv systemd|grep -F 'system/default.target '
# lrwxrwxrwx    1 root    root                       16 May 11 06:59 /usr/lib/systemd/system/default.target -> graphical.target
pysetjsonmember "default_target" '"multi-user.target"'
pysetjsonmember "machineid-compat" 'False'
pysetjsonmember "units" '["tuned.service"]'
# And test adding/removing files
pysetjsonmember "add-files" '[["foo.txt", "/usr/etc/foo.txt"],
                              ["baz.txt", "/usr/share/baz.txt"],
                              ["bar.txt", "/etc/bar.txt"]]'
pysetjsonmember "postprocess-script" \"$PWD/postprocess.sh\"
cat > postprocess.sh << EOF
#!/bin/bash
set -xeuo pipefail
echo misc-tweaks-postprocess-done > /usr/share/misc-tweaks-postprocess-done.txt
cp -a /usr/etc/foo.txt /usr/share/misc-tweaks-foo.txt
EOF
chmod a+x postprocess.sh

pysetjsonmember "remove-files" '["etc/hosts"]'
pysetjsonmember "remove-from-packages" '[["setup", "/etc/hosts\..*"]]'
rnd=$RANDOM
echo $rnd > composedata/foo.txt
echo bar > composedata/bar.txt
echo baz > composedata/baz.txt
# Test tmp-is-dir
pysetjsonmember "tmp-is-dir" 'True'

# Do the compose
runcompose
echo "ok compose"

# Tests for nodocs
ostree --repo=${repobuild} ls -R ${treeref} /usr/share/man > manpages.txt
assert_not_file_has_content manpages.txt man5/ostree.repo.5
echo "ok no manpages"

# Tests for units
ostree --repo=${repobuild} ls ${treeref} \
       /usr/lib/systemd/system/default.target > out.txt
assert_file_has_content out.txt '-> .*multi-user\.target'
echo "ok default target"

ostree --repo=${repobuild} ls ${treeref} \
       /usr/etc/systemd/system/multi-user.target.wants > out.txt
assert_file_has_content out.txt '-> .*/tuned.service'
echo "ok enable units"

# Tests for files
ostree --repo=${repobuild} cat ${treeref} /usr/etc/foo.txt > out.txt
assert_file_has_content out.txt $rnd
ostree --repo=${repobuild} cat ${treeref} /usr/etc/bar.txt > out.txt
assert_file_has_content out.txt bar
ostree --repo=${repobuild} cat ${treeref} /usr/share/baz.txt > out.txt
assert_file_has_content out.txt baz
# https://github.com/projectatomic/rpm-ostree/pull/997
ostree --repo=${repobuild} cat ${treeref} /usr/share/misc-tweaks-foo.txt > out.txt
assert_file_has_content out.txt $rnd
echo "ok add-files"

ostree --repo=${repobuild} ls ${treeref} /usr/etc > out.txt
assert_not_file_has_content out.txt '/usr/etc/hosts$'
echo "ok remove-files"

ostree --repo=${repobuild} ls ${treeref} /usr/etc > out.txt
assert_not_file_has_content out.txt '/usr/etc/hosts\.allow$'
assert_not_file_has_content out.txt '/usr/etc/hosts\.deny$'
echo "ok remove-from-packages"

# https://github.com/projectatomic/rpm-ostree/issues/669
ostree --repo=${repobuild} ls  ${treeref} /tmp > ls.txt
assert_file_has_content ls.txt 'd01777 0 0      0 /tmp'
echo "ok /tmp"

# https://github.com/projectatomic/rpm-ostree/pull/1425
ostree --repo=${repobuild} ls ${treeref} /usr/etc > ls.txt
assert_not_file_has_content ls.txt 'machine-id'
echo "ok machine-id"
