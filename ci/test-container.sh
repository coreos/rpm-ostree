#!/bin/bash
set -xeuo pipefail

fatal() {
    echo "$@" 1>&2
    exit 1
}

# Verify container flow
if rpm-ostree status 2>err.txt; then
    fatal "status in container"
fi
if ! grep -qe "error.*This system was not booted via libostree" err.txt; then
    cat err.txt
    fatal "did not find expected error"
fi

origindir=/etc/rpm-ostree/origin.d
mkdir -p "${origindir}"
cat > "${origindir}/clienterror.yaml" << 'EOF'
base-refspec: "foo/x86_64/bar"
EOF
if rpm-ostree ex rebuild 2>err.txt; then
   fatal "did rebuild with base-refspec"
fi

# Test the no-op rebuild path
rm "${origindir}/clienterror.yaml"
rpm-ostree ex rebuild

if ! test -x /usr/bin/yum; then
    rpm-ostree cliwrap install-to-root /
fi

# Test a critical path package
yum install cowsay && yum clean all
cowsay "It worked"
test '!' -d /var/cache/rpm-ostree

# Test overrides
versionid=$(grep -E '^VERSION_ID=' /etc/os-release)
versionid=${versionid:11} # trim off VERSION_ID=
case $versionid in
  35)
    url_suffix=2.13.0/5.fc35/x86_64/ignition-2.13.0-5.fc35.x86_64.rpm
    # 2.14.0
    koji_url=https://koji.fedoraproject.org/koji/buildinfo?buildID=1967838
    ;;
  36)
    url_suffix=2.13.0/5.fc36/x86_64/ignition-2.13.0-5.fc36.x86_64.rpm
    # 2.14.0
    koji_url=https://koji.fedoraproject.org/koji/buildinfo?buildID=1967836
    ;;
  *) fatal "Unsupported Fedora version: $versionid";;
esac
URL=https://kojipkgs.fedoraproject.org//packages/ignition/$url_suffix
# test replacement by URL
rpm-ostree override replace $URL
rpm-ostree override remove ignition
# test local RPM install
curl -Lo ignition.rpm $URL
rpm-ostree install ignition.rpm

# test replacement by Koji URL
rpm-ostree override replace $koji_url |& tee out.txt
n_downloaded=$(grep Downloading out.txt | wc -l)
if [[ $n_downloaded != 1 ]]; then
  fatal "Expected 1 'Downloading', but got $n_downloaded"
fi

echo ok
