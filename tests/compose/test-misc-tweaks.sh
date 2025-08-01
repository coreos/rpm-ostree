#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
# test `recommends: true` (test-basic[-unified] test the false path)
build_rpm foobar recommends foobar-rec
build_rpm foobar-rec
build_rpm quuz
build_rpm corge version 1.0
build_rpm corge version 2.0
build_rpm thirdpartymodules version 2.3 \
  files "/usr/lib/modules/4.7.8-10.x86_64/foo.ko
         /usr/lib/modules/4.9.3-2.x86_64/foo.ko
         /usr/lib/modules/5.8.1-11.x86_64/foo.ko
         " \
  install "for v in 4.7.8-10.x86_64 4.9.3-2.x86_64 5.8.1-11.x86_64; do d=%{buildroot}/usr/lib/modules/\$v; mkdir -p \$d && echo kmod\$v > \$d/foo.ko; done"
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
# Notice here we _don't_ add test-repo to the treefile in this test. Instead we
# set it to enabled in the repo file. This verifies that found yum repos enabled
# in their config are used by default when `repos:` is missing.
echo "enabled=1" >> yumrepo.repo
mv config/cache.repo .
echo "enabled=1" >> cache.repo
rm config/*.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
ln "$PWD/cache.repo" config/cache.repo
treefile_del "repos"
# the top-level manifest doesn't have any packages, so just set it
treefile_append "packages" $'["\'foobar >= 0.5\' quuz \'corge < 2.0\' barbar barbaz thirdpartymodules"]'

# With docs and recommends, also test multi includes
cat > config/documentation.yaml <<'EOF'
documentation: true
EOF
cat > config/other.yaml <<'EOF'
recommends: true
selinux-label-version: 1
readonly-executables: true
rpmdb-normalize: true
container-cmd:
  - /usr/bin/bash
opt-usrlocal: "root"
EOF
treefile_append "include" '["documentation.yaml", "other.yaml"]'
for x in 'recommends' 'documentation' 'readonly-executables' 'opt-usrlocal'; do
  treefile_del "$x"
done

# Test package exclusion
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
mkdir /run/somedir
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

# Also test finalize.d
mkdir config/finalize.d
cat >config/finalize.d/01-test.sh <<'EOF'
#!/bin/bash
set -xeuo pipefail
[[ "${RPMOSTREE_WORKDIR}" =~ config ]]
# Verify we can read our sourcedir
test -f "${RPMOSTREE_WORKDIR}/manifest.json"
touch usr/share/finalize-dot-d-test
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

# We're also going to test `sysusers: compose-forced` here. But since we're
# testing on Fedora in which it's already the default, we'll first _undo_ that
# default so that our `sysusers` knob becomes load-bearing. To do that, we add
# an RPM macro dropin which unsets the sysusers macro.
rm -rf tmp/rootfs/usr
mkdir -p tmp/rootfs/usr/lib/{rpm/macros.d,sysusers.d}
echo '%__systemd_sysusers %nil' > tmp/rootfs/usr/lib/rpm/macros.d/macros.disable-sysusers
echo 'g clevis 888' > tmp/rootfs/usr/lib/sysusers.d/sysusers-test.conf
ostree --repo="${repo}" commit --consume --no-xattrs --owner-uid=0 --owner-gid=0 -b sysusers --tree=dir=tmp/rootfs

cat >> ${new_treefile} <<EOF
ostree-layers:
  - testlayer-1
  - testlayer-2
  - testlayer-3
  - sysusers
ostree-override-layers:
  - testoverride-1
sysusers: compose-forced
EOF

export treefile=${new_treefile}

# Do the compose
runcompose
echo "ok compose"

ostree --repo=${repo} cat ${treeref} /usr/lib/group > group.txt
assert_file_has_content group.txt 'clevis:x:888'
echo "ok sysusers-test"

ostree --repo=${repo} ls -X ${treeref} /usr/etc/sysctl.conf > ls.txt
assert_file_has_content ls.txt ':system_conf_t:'
echo "ok selinux-label-version"

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

# Verify rpmdb-normalize
ostree --repo=${repo} ls -R ${treeref} /usr/share/rpm > db.txt
assert_file_has_content_literal db.txt rpmdb.sqlite
assert_not_file_has_content_literal db.txt rpmdb.sqlite-shm
echo "ok db"

ostree --repo=${repo} ls ${treeref} /opt > ls.txt
assert_file_has_content ls.txt '^d0'
ostree --repo=${repo} ls ${treeref} /usr/local > ls.txt
assert_file_has_content ls.txt '^d0' 
echo "ok opt-usrlocal"

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
