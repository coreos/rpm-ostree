#!/bin/bash
# SPDX-License-Identifier: LGPL-2.0-or-later
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

formats=(4)
# RPM 6.0 introduced v6 package generation. Older builders may know RPMFORMAT
# while still producing v4 packages when asked for v6.
rpm_supports_v6=$(rpmbuild --eval '%{expr: v"%{rpmversion}" >= v"6.0.0"}')
if [ "$rpm_supports_v6" = 1 ]; then
    formats+=(6)
fi

cat > "$treefile" <<'MANIFEST'
{
  "ref": "test/rpm-payload-format",
  "boot-location": "modules",
  "repos": ["test-repo"],
  "packages": [],
  "documentation": false,
  "selinux": false
}
MANIFEST

# Cross several reads of the payload stream and force an extended tar pathname.
dd if=/dev/zero of=expected bs=1024 count=160
printf 'end of payload\n' >> expected
long_name=$(printf '%0150d' 0)

for format in "${formats[@]}"; do
    name="testpkg-payload-v${format}"
    hardlink_install=
    hardlink_files=
    if [ "$format" = 6 ]; then
        # Each nonempty group has distinct content so brp-linkdupes cannot merge
        # a filtered group's payload with an unrelated, retained file.
        hardlink_install='
                  mkdir -p $data/{doc-first,doc-last,empty-first,empty-last,all-doc}
                  cat $data/data > $data/doc-first/a-doc
                  echo doc-first-content >> $data/doc-first/a-doc
                  ln $data/doc-first/a-doc $data/doc-first/b-doc
                  ln $data/doc-first/a-doc $data/doc-first/z-keep
                  echo doc-last-content > $data/doc-last/a-keep
                  ln $data/doc-last/a-keep $data/doc-last/z-doc
                  touch $data/empty-first/a-doc $data/empty-last/a-keep
                  chmod 0640 $data/empty-first/a-doc
                  chmod 0600 $data/empty-last/a-keep
                  ln $data/empty-first/a-doc $data/empty-first/z-keep
                  ln $data/empty-last/a-keep $data/empty-last/z-doc
                  echo all-doc-content > $data/all-doc/a-doc
                  ln $data/all-doc/a-doc $data/all-doc/z-doc
                  mkdir -p %{buildroot}/run/%{name}
                  echo run-content > %{buildroot}/run/%{name}/a-filtered
                  ln %{buildroot}/run/%{name}/a-filtered $data/run-keep
                  mkdir -p %{buildroot}/etc/selinux %{buildroot}/opt/%{name}
                  echo lock-content > %{buildroot}/etc/selinux/%{name}.LOCK
                  ln %{buildroot}/etc/selinux/%{name}.LOCK %{buildroot}/opt/%{name}/lock-keep'
        hardlink_files='
               %doc /usr/share/%{name}/doc-first/a-doc
               %doc /usr/share/%{name}/doc-first/b-doc
               /usr/share/%{name}/doc-first/z-keep
               /usr/share/%{name}/doc-last/a-keep
               %doc /usr/share/%{name}/doc-last/z-doc
               %doc /usr/share/%{name}/empty-first/a-doc
               /usr/share/%{name}/empty-first/z-keep
               /usr/share/%{name}/empty-last/a-keep
               %doc /usr/share/%{name}/empty-last/z-doc
               %doc /usr/share/%{name}/all-doc/a-doc
               %doc /usr/share/%{name}/all-doc/z-doc
               /run/%{name}/a-filtered
               /usr/share/%{name}/run-keep
               /etc/selinux/%{name}.LOCK
               /opt/%{name}/lock-keep'
    fi
    build_rpm "$name" rpmformat "$format" \
        provides "user(bin)" provides "group(bin)" \
        install ": > %{buildroot}/usr/bin/%{name}
                  data=%{buildroot}/usr/share/%{name}
                  mkdir -p \$data %{buildroot}/var/lib/%{name}
                  install -m 0640 '$PWD/expected' \$data/data
                  ln \$data/data \$data/${long_name}
                  ln -s data \$data/symlink
                  echo doc > \$data/doc
                  touch \$data/empty
                  $hardlink_install" \
        files "/usr/share/%{name}/data
               /usr/share/%{name}/${long_name}
               /usr/share/%{name}/symlink
               /usr/share/%{name}/empty
               %doc /usr/share/%{name}/doc
               %ghost /usr/share/%{name}/ghost
               %dir %attr(0750, bin, bin) /var/lib/%{name}
               $hardlink_files"
    if [ "$rpm_supports_v6" = 1 ]; then
        # Check the output too: requesting v6 must actually produce RPMFORMAT=6.
        assert_streq "$(rpm -qp --qf '%{RPMFORMAT}' yumrepo/packages/*/"${name}"-*.rpm)" "$format"
    fi
    treefile_append packages "[\"$name\"]"
done

echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
rpm-ostree compose tree "${treefile}" --unified-core --repo="${repo}" --cachedir=cache --download-only

cache_repo=cache/pkgcache-repo
for format in "${formats[@]}"; do
    name="testpkg-payload-v${format}"
    ref=$(ostree --repo="$cache_repo" refs --list "rpmostree/pkg/$name")
    data="/usr/share/$name"
    ostree --repo="$cache_repo" cat "$ref" "$data/data" > actual
    cmp expected actual
    ostree --repo="$cache_repo" cat "$ref" "$data/$long_name" > actual
    cmp expected actual
    ostree --repo="$cache_repo" cat "$ref" "$data/empty" > actual
    test ! -s actual
    ostree --repo="$cache_repo" ls "$ref" "$data/data" "$data/symlink" > files
    assert_file_has_content files "^-00640 .* $data/data$"
    assert_file_has_content_literal files "$data/symlink -> data"
    if ostree --repo="$cache_repo" ls "$ref" "$data/doc" || \
       ostree --repo="$cache_repo" ls "$ref" "$data/ghost"; then
        fatal "Unexpected doc or ghost in the RPM v${format} commit"
    fi
    ostree --repo="$cache_repo" cat "$ref" "/usr/lib/rpm-ostree/tmpfiles.d/$name.conf" > tmpfiles
    assert_file_has_content_literal tmpfiles "d /var/lib/$name 0750 bin bin - -"
    if [ "$format" = 6 ]; then
        for entry in doc-first/z-keep doc-last/a-keep run-keep; do
            ostree --repo="$cache_repo" cat "$ref" "$data/$entry" > actual
            case "$entry" in
                doc-first/*)
                    cat expected > expected-hardlink
                    echo doc-first-content >> expected-hardlink
                    ;;
                doc-last/*) echo doc-last-content > expected-hardlink ;;
                run-keep) echo run-content > expected-hardlink ;;
            esac
            cmp expected-hardlink actual
        done
        ostree --repo="$cache_repo" cat "$ref" "/usr/lib/opt/$name/lock-keep" > actual
        echo lock-content > expected-hardlink
        cmp expected-hardlink actual
        for entry in empty-first/z-keep empty-last/a-keep; do
            ostree --repo="$cache_repo" cat "$ref" "$data/$entry" > actual
            test ! -s actual
        done
        for path in "$data/doc-first/a-doc" "$data/doc-first/b-doc" "$data/doc-last/z-doc" \
                    "$data/empty-first/a-doc" "$data/empty-last/z-doc" \
                    "$data/all-doc/a-doc" "$data/all-doc/z-doc" \
                    "/run/$name/a-filtered" "/usr/etc/selinux/$name.LOCK"; do
            if ostree --repo="$cache_repo" ls "$ref" "$path"; then
                fatal "Unexpected filtered hardlink at $path"
            fi
        done
        echo "ok filtered RPM v6 hardlinks"
    fi
    echo "ok RPM v${format} payload"
done

# Reimport with docs enabled. Every name in the documentation groups must now
# exist, including the aliases which were filtered out during the first import.
if [ "$rpm_supports_v6" = 1 ]; then
    treefile_set documentation True
    mkdir cache-docs
    rpm-ostree compose tree "${treefile}" --unified-core --repo="${repo}" \
        --cachedir=cache-docs --download-only
    cache_repo=cache-docs/pkgcache-repo
    for entry in doc-first/a-doc doc-first/b-doc doc-first/z-keep \
                 doc-last/a-keep doc-last/z-doc all-doc/a-doc all-doc/z-doc; do
        ostree --repo="$cache_repo" cat "$ref" "$data/$entry" > actual
        case "$entry" in
            doc-first/*)
                cat expected > expected-hardlink
                echo doc-first-content >> expected-hardlink
                ;;
            doc-last/*) echo doc-last-content > expected-hardlink ;;
            all-doc/*) echo all-doc-content > expected-hardlink ;;
        esac
        cmp expected-hardlink actual
    done
    for entry in empty-first/a-doc empty-first/z-keep empty-last/a-keep empty-last/z-doc; do
        ostree --repo="$cache_repo" cat "$ref" "$data/$entry" > actual
        test ! -s actual
    done
    echo "ok RPM v6 hardlinks with documentation enabled"
fi
