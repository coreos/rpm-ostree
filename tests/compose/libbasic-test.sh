# This used to live in test-basic.sh, but it's now shared with test-basic-unified.sh
basic_test() {
if ostree --repo=${repo} ls -R ${treeref} /usr/etc/passwd-; then
    assert_not_reached "Found /usr/etc/passwd- backup file in tree"
fi
echo "ok passwd no backups"

validate_passwd() {
    f=$1
    shift
    ostree --repo=${repo} cat ${treeref} /usr/lib/$f |grep -v '^root' | sort > $f.tree
    cat config/$f | while read line; do
        if ! grep -q "$line" "$f.tree"; then
            echo "Missing entry: %line"
        fi
    done
}

validate_passwd passwd
validate_passwd group

ostree --repo=${repo} cat ${treeref} /usr/etc/default/useradd > useradd.txt
assert_file_has_content_literal useradd.txt HOME=/var/home

ostree --repo=${repo} cat ${treeref} \
  /usr/etc/selinux/targeted/contexts/files/file_contexts.homedirs > homedirs.txt
assert_file_has_content homedirs.txt '^/var/home'

ostree --repo=${repo} cat ${treeref} \
  /usr/etc/selinux/targeted/contexts/files/file_contexts.subs_dist > subs_dist.txt
assert_not_file_has_content subs_dist.txt '^/var/home \+'
assert_file_has_content subs_dist.txt '^/home \+/var/home$'
echo "ok etc/default/useradd"

for path in /usr/share/rpm /usr/lib/sysimage/rpm-ostree-base-db; do
    ostree --repo=${repo} ls -R ${treeref} ${path} > db.txt
    assert_file_has_content_literal db.txt /Packages
done
echo "ok db"

ostree --repo=${repo} show --print-metadata-key exampleos.gitrepo ${treeref} > meta.txt
assert_file_has_content meta.txt 'rev.*97ec21c614689e533d294cdae464df607b526ab9'
assert_file_has_content meta.txt 'src.*https://gitlab.com/exampleos/custom-atomic-host'
ostree --repo=${repo} show --print-metadata-key exampleos.tests ${treeref} > meta.txt
assert_file_has_content meta.txt 'smoketested.*e2e'
ostree --repo=${repo} show --print-metadata-key rpmostree.rpmmd-repos ${treeref} > meta.txt
assert_file_has_content meta.txt 'id.*cache.*timestamp'
ostree --repo=${repo} show --print-metadata-key foobar  ${treeref} > meta.txt
assert_file_has_content meta.txt 'bazboo'
ostree --repo=${repo} show --print-metadata-key overrideme  ${treeref} > meta.txt
assert_file_has_content meta.txt 'new val'
echo "ok metadata"

ostree --repo=${repo} ls -R ${treeref} /usr/lib/modules | grep -v /kernel/ > ls.txt
assert_file_has_content ls.txt '/vmlinuz$'
assert_file_has_content ls.txt '^-00644 .*/initramfs.img$'
echo "ok kernel and initramfs"

ostree --repo=${repo} ls ${treeref} /usr/share > share.txt
assert_not_file_has_content share.txt /usr/share/man
# test-misc-tweaks tests the docs path
echo "ok no manpages"

# https://github.com/projectatomic/rpm-ostree/pull/1425
ostree --repo=${repo} ls ${treeref} /usr/etc > ls.txt
assert_not_file_has_content ls.txt 'machine-id'
# test-misc-tweaks tests the machine-id compat path
echo "ok no machine-id"

ostree --repo=${repo} ls ${treeref} usr/etc/systemd/system/multi-user.target.wants/chronyd.service > preset.txt
assert_file_has_content_literal preset.txt '-> /usr/lib/systemd/system/chronyd.service'
echo "ok systemctl preset"

ostree --repo=${repo} ls -X ${treeref} usr/bin/docker > docker.txt
assert_file_has_content_literal docker.txt 'system_u:object_r:container_runtime_exec_t:s0'
echo "ok container-selinux"

ostree --repo=${repo} ls ${treeref} /usr/bin/su > su.txt
assert_file_has_content su.txt '^-04[71][0-7][0-7]'
echo "ok setuid"

ostree --repo=${repo} ls -X ${treeref} /usr/bin/ping > ping.txt
assert_file_has_content_literal ping.txt "b'security.capability', [byte"
echo "ok fcaps"

# https://github.com/projectatomic/rpm-ostree/issues/669
ostree --repo=${repo} ls  ${treeref} /tmp > ls.txt
assert_file_has_content ls.txt 'd01777 0 0      0 /tmp'
echo "ok /tmp"

ostree --repo=${repo} ls ${treeref} /usr/share/rpm > ls.txt
assert_not_file_has_content ls.txt '__db' 'lock'
ostree --repo=${repo} ls -R ${treeref} /usr/etc/selinux > ls.txt
assert_not_file_has_content ls.txt 'LOCK'
echo "ok no leftover files"

ostree --repo=${repo} show ${treeref} \
  --print-metadata-key rpmostree.rpmdb.pkglist > pkglist.txt
assert_file_has_content pkglist.txt 'systemd'
assert_file_has_content_literal pkglist.txt 'foobar'
assert_not_file_has_content pkglist.txt 'foobar-rec'
echo "ok compose pkglist"

ostree --repo=${repo} cat ${treeref} /usr/share/rpm-ostree/treefile.json > treefile.json
assert_jq treefile.json '.basearch == "x86_64"'
echo "ok basearch"

ostree --repo=${repo} rev-parse ${treeref}^ > parent.txt
assert_file_has_content parent.txt 01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b
echo "ok --parent"
}

