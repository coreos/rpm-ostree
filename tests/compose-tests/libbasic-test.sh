# This used to live in test-basic.sh, but it's now shared with test-basic-unified.sh
basic_test() {
if ostree --repo=${repobuild} ls -R ${treeref} /usr/etc/passwd-; then
    assert_not_reached "Found /usr/etc/passwd- backup file in tree"
fi
echo "ok passwd no backups"

validate_passwd() {
    f=$1
    shift
    ostree --repo=${repobuild} cat ${treeref} /usr/lib/$f |grep -v '^root' | sort > $f.tree
    cat composedata/$f | while read line; do
        if ! grep -q "$line" "$f.tree"; then
            echo "Missing entry: %line"
        fi
    done
}

validate_passwd passwd
validate_passwd group


ostree --repo=${repobuild} cat ${treeref} /usr/etc/default/useradd > useradd.txt
assert_file_has_content_literal useradd.txt HOME=/var/home

ostree --repo=${repobuild} cat ${treeref} \
  /usr/etc/selinux/targeted/contexts/files/file_contexts.homedirs > homedirs.txt
assert_file_has_content homedirs.txt '^/var/home'

ostree --repo=${repobuild} cat ${treeref} \
  /usr/etc/selinux/targeted/contexts/files/file_contexts.subs_dist > subs_dist.txt
assert_not_file_has_content subs_dist.txt '^/var/home \+'
assert_file_has_content subs_dist.txt '^/home \+/var/home$'
echo "ok etc/default/useradd"

for path in /usr/share/rpm /usr/lib/sysimage/rpm-ostree-base-db; do
    ostree --repo=${repobuild} ls -R ${treeref} ${path} > db.txt
    assert_file_has_content_literal db.txt /Packages
done
echo "ok db"

ostree --repo=${repobuild} show --print-metadata-key exampleos.gitrepo ${treeref} > meta.txt
assert_file_has_content meta.txt 'rev.*97ec21c614689e533d294cdae464df607b526ab9'
assert_file_has_content meta.txt 'src.*https://gitlab.com/exampleos/custom-atomic-host'
ostree --repo=${repobuild} show --print-metadata-key exampleos.tests ${treeref} > meta.txt
assert_file_has_content meta.txt 'smoketested.*e2e'
ostree --repo=${repobuild} show --print-metadata-key rpmostree.rpmmd-repos ${treeref} > meta.txt
assert_file_has_content meta.txt 'id.*fedora.*timestamp'
echo "ok metadata"

for path in /boot /usr/lib/ostree-boot; do
    ostree --repo=${repobuild} ls -R ${treeref} ${path} > bootls.txt
    assert_file_has_content bootls.txt vmlinuz-
    assert_file_has_content bootls.txt initramfs-
    echo "ok boot files"
done
vmlinuz_line=$(grep -o '/vmlinuz.*$' bootls.txt)
kver=$(echo ${vmlinuz_line} | sed -e 's,^/vmlinuz-,,' -e 's,-[0-9a-f]*$,,')
ostree --repo=${repobuild} ls ${treeref} /usr/lib/modules/${kver}/{vmlinuz,initramfs.img} >/dev/null

ostree --repo=${repobuild} ls -R ${treeref} /usr/share/man > manpages.txt
assert_file_has_content manpages.txt man5/ostree.repo.5
echo "ok manpages"

# https://github.com/projectatomic/rpm-ostree/pull/1425
ostree --repo=${repobuild} ls ${treeref} /usr/etc/machine-id
echo "ok machine-id"

ostree --repo=${repobuild} ls ${treeref} usr/etc/systemd/system/multi-user.target.wants/chronyd.service > preset.txt
assert_file_has_content_literal preset.txt '-> /usr/lib/systemd/system/chronyd.service'
echo "ok systemctl preset"

ostree --repo=${repobuild} ls -X ${treeref} usr/bin/docker-current > docker.txt
assert_file_has_content_literal docker.txt 'system_u:object_r:container_runtime_exec_t:s0'
echo "ok container-selinux"

ostree --repo=${repobuild} ls ${treeref} /usr/bin/su > su.txt
assert_file_has_content su.txt '^-04[71][0-7][0-7]'
echo "ok setuid"

ostree --repo=${repobuild} ls -X ${treeref} /usr/bin/ping > ping.txt
assert_file_has_content_literal ping.txt "b'security.capability', [byte"
echo "ok fcaps"

# https://github.com/projectatomic/rpm-ostree/issues/669
ostree --repo=${repobuild} ls  ${treeref} /tmp > ls.txt
assert_file_has_content ls.txt 'l00777 0 0      0 /tmp -> sysroot/tmp'
echo "ok /tmp"

ostree --repo=${repobuild} ls ${treeref} /usr/share/rpm > ls.txt
assert_not_file_has_content ls.txt '__db' 'lock'
ostree --repo=${repobuild} ls -R ${treeref} /usr/etc/selinux > ls.txt
assert_not_file_has_content ls.txt 'LOCK'
echo "ok no leftover files"

ostree --repo=${repobuild} show ${treeref} \
  --print-metadata-key rpmostree.rpmdb.pkglist > pkglist.txt
assert_file_has_content pkglist.txt 'systemd'
# This is currently a Recommends: package.  If you change this, please
# also change the corresponding test in misc-tweaks.sh.
assert_file_has_content pkglist.txt 'systemd-bootchart'
echo "ok compose pkglist"
}

ostree --repo=${repobuild} cat ${treeref} /usr/share/rpm-ostree/treefile.json > treefile.json
assert_jq treefile.json '.basearch == "x86_64"'
echo "ok basearch"
