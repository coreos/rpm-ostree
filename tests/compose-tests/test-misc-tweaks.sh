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
pysetjsonmember "recommends" 'False'
pysetjsonmember "units" '["tuned.service"]'
# And test adding/removing files
pysetjsonmember "add-files" '[["foo.txt", "/usr/etc/foo.txt"],
                              ["baz.txt", "/usr/share/baz.txt"],
                              ["bar.txt", "/etc/bar.txt"]]'
pysetjsonmember "postprocess-script" \"$PWD/postprocess.sh\"
pysetjsonmember "postprocess" '["""#!/bin/bash
touch /usr/share/postprocess-testing""",
"""#!/bin/bash
set -xeuo pipefail
touch /usr/share/included-postprocess-test
rm /usr/share/postprocess-testing
touch /usr/share/postprocess-testing-done"""]'
cat > postprocess.sh << EOF
#!/bin/bash
set -xeuo pipefail
# Ordering should be after postprocess
rm /usr/share/postprocess-testing-done
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

new_treefile=composedata/fedora-misc-tweaks-includer.yaml
cat > ${new_treefile} <<EOF
include: $(basename ${treefile})
postprocess:
 - |
   #!/bin/bash
   set -xeuo pipefail
   test -f /usr/share/included-postprocess-test
EOF

for x in $(seq 3); do
  rm tmp/usr -rf
  mkdir -p tmp/usr/{bin,share}
  mkdir tmp/usr/share/testsubdir-${x}
  echo sometest${x} > tmp/usr/bin/sometestbinary-${x}
  chmod a+x tmp/usr/bin/sometestbinary-${x}
  echo sometestdata${x} > tmp/usr/share/sometestdata-${x}
  echo sometestdata-subdir-${x} > tmp/usr/share/testsubdir-${x}/test
  ostree --repo="${repobuild}" commit --consume --no-xattrs --owner-uid=0 --owner-gid=0 -b testlayer-${x} --tree=dir=tmp
done
rm tmp/usr -rf
mkdir -p tmp/usr/share/info
echo some info | gzip > tmp/usr/share/info/bash.info.gz
ostree --repo="${repobuild}" commit --consume --no-xattrs --owner-uid=0 --owner-gid=0 -b testoverride-1 --tree=dir=tmp
cat >> ${new_treefile} <<EOF
ostree-layers:
  - testlayer-1
  - testlayer-2
  - testlayer-3
ostree-override-layers:
  - testoverride-1
EOF

export treefile=${new_treefile}


# Do the compose
compose_base_argv="${compose_base_argv} --unified-core"
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

ostree --repo=${repobuild} show ${treeref} \
       --print-metadata-key rpmostree.rpmdb.pkglist > pkglist.txt
# This is currently a Recommends: package.  If you change this, please
# also change the corresponding test in libbasic-test.sh.
assert_file_has_content_literal pkglist.txt 'systemd-'
assert_not_file_has_content pkglist.txt 'systemd-bootchart'
echo "ok recommends"

# Test overlays/overrides
for x in $(seq 3); do
  ostree --repo=${repobuild} cat ${treeref} /usr/bin/sometestbinary-${x} > t
  assert_file_has_content t "sometest${x}"
  ostree --repo=${repobuild} cat ${treeref} /usr/share/testsubdir-${x}/test > t
  assert_file_has_content t sometestdata-subdir-${x}
done
ostree --repo=${repobuild} cat ${treeref} /usr/share/info/bash.info.gz | gunzip > bash.info
assert_file_has_content bash.info 'some info'
echo "ok layers"

# Check that add-files with bad paths are rejected
prepare_compose_test "add-files-failure"
pysetjsonmember "add-files" '[["foo.txt", "/var/lib/foo.txt"]]'

# Do the compose ourselves since set -e doesn't work in function calls in if
rm ${compose_workdir} -rf
mkdir ${test_tmpdir}/workdir
if rpm-ostree compose tree ${compose_base_argv} ${treefile} |& tee err.txt; then
    assert_not_reached err.txt "Successfully composed with add-files for /var/lib?"
fi
assert_file_has_content_literal err.txt "Unsupported path in add-files: /var"
echo "ok bad add-files"
