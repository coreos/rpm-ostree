#!/bin/bash
# SPDX-License-Identifier: LGPL-2.0-or-later
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source-path=SCRIPTDIR
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

formats=(4)
if rpm --querytags | grep -Fx RPMFORMAT; then
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
    build_rpm "$name" rpmformat "$format" \
        provides "user(bin)" provides "group(bin)" \
        install ": > %{buildroot}/usr/bin/%{name}
                  data=%{buildroot}/usr/share/%{name}
                  mkdir -p \$data %{buildroot}/var/lib/%{name}
                  install -m 0640 '$PWD/expected' \$data/data
                  ln \$data/data \$data/${long_name}
                  ln -s data \$data/symlink
                  echo doc > \$data/doc
                  touch \$data/empty" \
        files "/usr/share/%{name}/data
               /usr/share/%{name}/${long_name}
               /usr/share/%{name}/symlink
               /usr/share/%{name}/empty
               %doc /usr/share/%{name}/doc
               %ghost /usr/share/%{name}/ghost
               %dir %attr(0750, bin, bin) /var/lib/%{name}"
    if [ "${#formats[@]}" -gt 1 ]; then
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
    echo "ok RPM v${format} payload"
done
