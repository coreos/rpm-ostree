#!/bin/bash
set -xeuo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=libcomposetest.sh
. "${dn}/libcomposetest.sh"

# Add a local rpm-md repo so we can mutate local test packages
treefile_append "repos" '["test-repo"]'

# An IMA signed RPM
build_rpm test-ima-signed \
             build "echo test-ima-signed-binary > %{name}" \
             install "mkdir -p %{buildroot}/usr/bin
                      install %{name} %{buildroot}/usr/bin" \
             files "/usr/bin/%{name}"
cd "${test_tmpdir}"
cat > genkey.config << 'EOF'
[ req ]
default_bits = 3048
distinguished_name = req_distinguished_name
prompt = no
string_mask = utf8only
x509_extensions = myexts
[ req_distinguished_name ]
O = Test
CN = Test key
emailAddress = example@example.com
[ myexts ]
basicConstraints=critical,CA:FALSE
keyUsage=digitalSignature
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
EOF
openssl req -new -nodes -utf8 -sha256 -days 36500 -batch \
            -x509 -config genkey.config \
            -outform DER -out ima.der -keyout privkey_ima.pem
export GNUPGHOME=${commondir}/../gpghome
export GPG_TTY=""
rpmsign --addsign --key-id "${TEST_GPG_KEYID_1}" --signfiles --fskpath=privkey_ima.pem yumrepo/packages/$(arch)/test-ima-signed*.rpm

echo gpgcheck=0 >> yumrepo.repo
ln "$PWD/yumrepo.repo" config/yumrepo.repo
treefile_append "packages" '["test-ima-signed"]'
treefile_pyedit "tf['ima'] = True"

runcompose

ostree --repo="${repo}" ls -X "${treeref}" /usr/bin/test-ima-signed > ima.txt
# It'd be good to also verify the file signature, try booting it etc.  But
# this is just a sanity check for now.
assert_file_has_content_literal ima.txt "(b'security.ima', [byte 0x"
echo "ok ima signature"
