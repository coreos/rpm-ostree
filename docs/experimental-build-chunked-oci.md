---
nav_order: 5
---

# experimental compose build-chunked-oci

Currently this project supports `rpm-ostree compose image` which is a highly
opinionated tool which consumes treefiles and outputs an OCI archive.

However, it does not support common container-native workflows such
as copying content from a distinct container image.

The `rpm-ostree experimental compose build-chunked-oci` command
accepts an arbitrary root filesystem, and synthesizes an OSTree-based
container from it.

At the current time, it is recommended that the input
root filesystem be derived from a reference maintained base image,
such as the fedora-bootc ones. Especially if you are
targeting bootc systems with this, please trakc

## Example as part of a Containerfile

This relies on a podman-ecosystem specific feature: `FROM oci:`
which allows ingesting into the container build flow an OCI
archive built inside a stage. With this, we can generate
arbitrary container structure, in particular "chunked"
images. A bit more in [container.md](container).

In this example, we will dramatically trim out the current reference
base image, including especially the rpm-ostree and dnf stacks.

```Dockerfile
FROM quay.io/fedora/fedora-bootc:rawhide as rootfs
RUN <<EORUN
set -xeuo pipefail
# Remove some high level superfulous stuff
dnf -y remove sos NetworkManager-tui vim nano
# And this only targets VMs, so flush out all firmware
rpm -qa --queryformat=%{NAME} | grep -Fe '-firmware-' | xargs dnf -y remove 
# We don't want any python, and we don't need rpm-ostree either.
dnf -y remove python3 rpm-ostree{,-libs}
bootc container lint
EORUN

# This builder image can be anything as long as it has a new enough
# rpm-ostree.
FROM quay.io/fedora/fedora-bootc:rawhide as builder
RUN --mount=type=bind,rw=true,src=.,dst=/buildcontext,bind-propagation=shared \
    --mount=from=rootfs,dst=/rootfs <<EORUN
set -xeuo pipefail
rm /buildcontext/out.oci -rf
rpm-ostree experimental compose build-chunked-oci --bootc --format-version=1 \
           --rootfs=/rootfs --output /buildcontext/out.oci
EORUN

# Finally, output the OCI archive back into our final container image. Here we
# can add labels and other metadata - note that no metadata was inherited from
# the source image - only the root filesystem!
FROM oci:./out.oci
# Need to reference builder here to force ordering. But since we have to run
# something anyway, we might as well cleanup after ourselves.
RUN --mount=type=bind,from=builder,src=.,target=/var/tmp \
    --mount=type=bind,rw=true,src=.,dst=/buildcontext,bind-propagation=shared rm /buildcontext/out.oci -rf
```

## Using outside of container builds

There is no requirement to run as part of a container build, or even a container.
You can generate a root filesystem however you want, and get an OCI archive
out, which can be pushed directly to a registry using a tool such as `skopeo`.

```
mkdir -p rootfs
...
rpm-ostree experimental compose build-chunked-oci --bootc --format-version=1 \
           --rootfs=rootfs --output out.oci
skopeo copy --authfile=/path/to/auth.json oci:out.oci docker://quay.io/exampleos/exampleos:latest
```

However as noted above, it is recommended to follow e.g. the
[fedora-bootc documentation](https://docs.fedoraproject.org/en-US/bootc/) around custom base images.
