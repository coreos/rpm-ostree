---
nav_order: 5
---

# experimental compose build-chunked-oci

Currently this project supports `rpm-ostree compose image` which is a highly
opinionated tool which consumes treefiles and outputs an OCI archive.

However, it does not support common container-native workflows such
as copying content from a distinct container image.

The `rpm-ostree experimental compose build-chunked-oci` command
accepts an arbitrary input container image, and synthesizes a
bootc (ostree container) ready image from it.

At the current time, it is recommended that the input
container image be derived from a reference maintained base image,
such as the fedora-bootc ones. Especially if you are
targeting bootc systems with this, please follow
<https://gitlab.com/fedora/bootc/tracker/-/issues/32>.

## Running

This command expects a container image already fetched into a `containers-storage:`
instance, and can output to `containers-storage:` or `oci`. 

```
podman build -t quay.io/exampleos/exampleos:build ...
...
rpm-ostree experimental compose build-chunked-oci --bootc --format-version=1 \
           --from=quay.io/exampleos/exampleos:build --output containers-storage:quay.io/exampleos/exampleos:latest
podman push quay.io/exampleos/exampleos:latest
```
