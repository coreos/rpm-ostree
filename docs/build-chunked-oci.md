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
exactly one is required.  Currently the `--bootc` option is required.
The option `--format-version` can be either `1` or `2`, and if
omitted, defaults to `1`.

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

### Using `--format-version`

The value of `--format-version` must be either `1` or `2`.  Additional
format versions may be added in the future.

A value of `1` will create an image with "sparse" layers.  A sparse
layer will contain information for a changed file `/path/to/foo` in
the tar stream, but may not contain information for the parent
directories `/path` and `/path/to`.  This has the advantage of
minimally reducing the size of the image since the tar stream is
smaller, but has the disadvantage that the directories must be
implicitly created when unpacking the layer.  This implicit creation
results in directories with unpredictable metadata and breaks
reproducible builds.

A value of `2` will ensure that for each layer, any parent directories
are explicitly defined in the tar stream for that layer.  This
increases the layer size, but removes ambiguity about the expected
metadata for the parent directories.

The default value is `--format-version=1` for backwards-compatibility
to ensure that images previously built with `--format-version=1` can
be updated while also reusing existing layers from the previous
version of an image.

If reproducible builds are desirable, it is recommended to use
`--format-version=2`.
