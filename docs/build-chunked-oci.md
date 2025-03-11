---
nav_order: 5
---

# compose build-chunked-oci

Currently this project supports `rpm-ostree compose image` which is a highly
opinionated tool which consumes treefiles and outputs an OCI archive.

However, it does not support common container-native workflows such
as copying content from a distinct container image.

The `rpm-ostree compose build-chunked-oci` command
accepts exactly one of:

- an arbitrary input container image
- a source root filesystem tree

and synthesizes a bootc (ostree container) ready image from it.  This
produces a chunked output image via the [same process](https://coreos.github.io/rpm-ostree/container/#creating-chunked-images)
as `rpm-ostree compose container-encapsulate`.

At the current time, when using an input container image, it is
recommended that the input container image be derived from a reference
maintained base image, such as the fedora-bootc ones. Especially if
you are targeting bootc systems with this, please follow
<https://gitlab.com/fedora/bootc/tracker/-/issues/32>.

## Running

Note that the `--from` and `--rootfs` options are mutually-exclusive;
exactly one is required.  Currently both `--bootc` and
`--format-version=1` are required options.  Additional format versions
may be added in the future.

### Using `--from`

This expects a container image already fetched into a `containers-storage:`
instance, and can output to `containers-storage:` or `oci`. 

```
podman build -t quay.io/exampleos/exampleos:build ...
...
rpm-ostree compose build-chunked-oci --bootc --format-version=1 \
           --from=quay.io/exampleos/exampleos:build --output containers-storage:quay.io/exampleos/exampleos:latest
podman push quay.io/exampleos/exampleos:latest
```

### Using `--rootfs`

This expects a source root filesystem tree, such as one created with
`rpm-ostree compose rootfs`.

```
# assumes package system configuration in /repos
rpm-ostree compose rootfs --source-root=/repos /path/to/input_manifest.yml /path/to/target_rootfs
rpm-ostree compose build-chunked-oci --bootc --format-version=1 --rootfs=/path/to/target_rootfs \
           --output containers-storage:quay.io/exampleos/exampleos:latest
podman push quay.io/exampleos/exampleos:latest
```
