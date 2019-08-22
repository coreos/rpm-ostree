#!/usr/bin/bash
# Build rpm-ostree, using cosa as a buildroot and then
# override the version inside cosa, then build FCOS
set -xeuo pipefail

cosaimg=quay.io/coreos-assembler/coreos-assembler:latest
podman pull "${cosaimg}"

# Build rpm-ostree using cosa as a buildroot, and extract the result
podman run --security-opt label=disable --rm \
       -v $(pwd):/srv/code -w /srv/code \
       --entrypoint bash --user root \
       "${cosaimg}" \
       -c 'yum -y swap fedora-release-container fedora-release && ./ci/build.sh && make install DESTDIR=$(pwd)/installroot'

codedir=$(pwd)
mkdir fcos
cd fcos
cat >script.sh <<'EOF'
#!/usr/bin/bash
set -xeuo pipefail
# Overlay the built binaries
rsync -rlv /code/installroot/usr/ /usr/
coreos-assembler init --force https://github.com/coreos/fedora-coreos-config
coreos-assembler build ostree
EOF
chmod a+x script.sh
podman run --privileged --rm -ti \
       -v ${codedir}:/code -v $(pwd):/srv -w /srv \
       --entrypoint bash \
       --privileged ${cosaimg} \
       ./script.sh
