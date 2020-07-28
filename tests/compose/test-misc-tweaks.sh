#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
treefile_append "repos" '["test-repo"]'
# test `recommends: true` (test-basic[-unified] test the false path)
build_rpm foobar recommends foobar-rec
build_rpm foobar-rec
build_rpm quuz
build_rpm corge version 1.0
build_rpm corge version 2.0
# test `remove-from-packages` (files shared with other pkgs should not be removed)
build_rpm barbar \
          files "/etc/sharedfile
                 /usr/bin/barbarextra" \
          install "mkdir -p %{buildroot}/etc && echo shared file data > %{buildroot}/etc/sharedfile
                   mkdir -p %{buildroot}/usr/bin && echo more binary data > %{buildroot}/usr/bin/barbarextra"
build_rpm barbaz \
          files "/etc/sharedfile" \
          install "mkdir -p %{buildroot}/etc && echo shared file data > %{buildroot}/etc/sharedfile"

echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
# the top-level manifest doesn't have any packages, so just set it
treefile_append "packages" $'["\'foobar >= 0.5\' quuz \'corge < 2.0\' barbar barbaz"]'

# With docs and recommends, also test multi includes
cat > config/documentation.yaml <<'EOF'
documentation: true
EOF
cat > config/other.yaml <<'EOF'
recommends: true
readonly-executables: true
EOF
treefile_append "include" '["documentation.yaml", "other.yaml"]'
for x in 'recommends' 'documentation' 'readonly-executables'; do
  treefile_del "$x"
done

# Test blacklists
treefile_append "exclude-packages" '["somenonexistent-package", "gnome-shell"]'

# Note this overrides:
# $ rpm -q systemd
# systemd-245.4-1.fc32.x86_64
# $ rpm -qlv systemd | grep -F 'system/default.target'
# lrwxrwxrwx    1 root     root                       16 Apr  1 17:46 /usr/lib/systemd/system/default.target -> graphical.target
treefile_set "default-target" '"multi-user.target"'
treefile_append "units" '["zincati.service"]'
# Need this in order to test unit enablement
treefile_set "machineid-compat" "True"
# And test adding/removing files
treefile_append "add-files" '[["foo.txt", "/usr/etc/foo.txt"],
                           ["baz.txt", "/usr/share/baz.txt"],
                           ["bar.txt", "/etc/bar.txt"]]'
treefile_set "postprocess-script" \"$PWD/postprocess.sh\"
treefile_set "postprocess" '["""#!/bin/bash
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

treefile_set "remove-files" '["etc/hosts"]'
treefile_set "remove-from-packages" '[["barbar", "/usr/bin/*"],
                                      ["barbar", "/etc/sharedfile"]]'
rnd=$RANDOM
echo $rnd > config/foo.txt
echo bar >  config/bar.txt
echo baz >  config/baz.txt
# Test tmp-is-dir False
treefile_set "tmp-is-dir" 'False'

new_treefile=config/fedora-misc-tweaks-includer.yaml
cat > ${new_treefile} <<EOF
include: $(basename ${treefile})
postprocess:
 - |
   #!/bin/bash
   set -xeuo pipefail
   test -f /usr/share/included-postprocess-test
EOF

mkdir -p tmp/rootfs
for x in $(seq 3); do
  rm tmp/rootfs/usr -rf
  mkdir -p tmp/rootfs/usr/{bin,share}
  mkdir tmp/rootfs/usr/share/testsubdir-${x}
  echo sometest${x} > tmp/rootfs/usr/bin/sometestbinary-${x}
  chmod a+x tmp/rootfs/usr/bin/sometestbinary-${x}
  echo sometestdata${x} > tmp/rootfs/usr/share/sometestdata-${x}
  echo sometestdata-subdir-${x} > tmp/rootfs/usr/share/testsubdir-${x}/test
  ostree --repo="${repo}" commit --consume --no-xattrs --owner-uid=0 --owner-gid=0 -b testlayer-${x} --tree=dir=tmp/rootfs
done
rm tmp/rootfs/usr -rf

mkdir -p tmp/rootfs/usr/{share/info,bin}
echo sweet new ls binary > tmp/rootfs/usr/bin/ls
ostree --repo="${repo}" commit --consume --no-xattrs --owner-uid=0 --owner-gid=0 -b testoverride-1 --tree=dir=tmp/rootfs
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
runcompose
echo "ok compose"

# Tests for docs
ostree --repo=${repo} ls -R ${treeref} /usr/share/man > manpages.txt
assert_file_has_content manpages.txt man5/ostree.repo.5
echo "ok manpages"

# Tests for units
ostree --repo=${repo} ls ${treeref} \
       /usr/lib/systemd/system/default.target > out.txt
assert_file_has_content out.txt '-> .*multi-user\.target'
echo "ok default target"

ostree --repo=${repo} ls ${treeref} \
       /usr/etc/systemd/system/multi-user.target.wants > out.txt
assert_file_has_content out.txt '-> .*/zincati.service'
echo "ok enable units"

# Tests for files
ostree --repo=${repo} cat ${treeref} /usr/etc/foo.txt > out.txt
assert_file_has_content out.txt $rnd
ostree --repo=${repo} cat ${treeref} /usr/etc/bar.txt > out.txt
assert_file_has_content out.txt bar
ostree --repo=${repo} cat ${treeref} /usr/share/baz.txt > out.txt
assert_file_has_content out.txt baz
# https://github.com/projectatomic/rpm-ostree/pull/997
ostree --repo=${repo} cat ${treeref} /usr/share/misc-tweaks-foo.txt > out.txt
assert_file_has_content out.txt $rnd
echo "ok add-files"

ostree --repo=${repo} ls ${treeref} /usr/etc > out.txt
assert_not_file_has_content out.txt '/usr/etc/hosts$'
echo "ok remove-files"

ostree --repo=${repo} ls ${treeref} /usr/bin > out.txt
assert_not_file_has_content out.txt 'bin/barbar'
assert_not_file_has_content out.txt 'bin/barbarextra'
ostree --repo=${repo} ls ${treeref} /usr/etc > out.txt
assert_file_has_content out.txt 'etc/sharedfile'
echo "ok remove-from-packages"

# https://github.com/projectatomic/rpm-ostree/issues/669
ostree --repo=${repo} ls  ${treeref} /tmp > ls.txt
assert_file_has_content ls.txt 'l00777 0 0      0 /tmp -> sysroot/tmp'
echo "ok /tmp"

rpm-ostree db list --repo=${repo} ${treeref} > pkglist.txt
assert_file_has_content_literal pkglist.txt 'foobar'
assert_file_has_content_literal pkglist.txt 'foobar-rec'
echo "ok recommends"

assert_file_has_content_literal pkglist.txt 'quuz'
assert_file_has_content_literal pkglist.txt 'corge-1.0'
assert_not_file_has_content_literal pkglist.txt 'corge-2.0'
echo "ok package versions"

# Test overlays/overrides
for x in $(seq 3); do
  ostree --repo=${repo} cat ${treeref} /usr/bin/sometestbinary-${x} > t
  assert_file_has_content t "sometest${x}"
  ostree --repo=${repo} cat ${treeref} /usr/share/testsubdir-${x}/test > t
  assert_file_has_content t sometestdata-subdir-${x}
done
ostree --repo=${repo} cat ${treeref} /usr/bin/ls > ls.txt
assert_file_has_content ls.txt '^sweet new ls binary$'
echo "ok layers"

# Test readonly-executables
ostree --repo=${repo} ls ${treeref} /usr/bin/bash > ls.txt
assert_file_has_content ls.txt '^-00555 .*/usr/bin/bash$'
echo "ok readonly-executables"

# Check that add-files with bad paths are rejected
treefile_append "add-files" '[["foo.txt", "/var/lib/foo.txt"]]'

if runcompose |& tee err.txt; then
    assert_not_reached "Successfully composed with add-files for /var/lib?"
fi
assert_file_has_content_literal err.txt "Unsupported path in add-files: /var"
echo "ok bad add-files"
