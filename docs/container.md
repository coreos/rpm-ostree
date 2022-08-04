---
parent: Experimental features
nav_order: 1
---

# ostree native containers

For more information on this, see [CoreOS layering](https://github.com/coreos/enhancements/pull/7).

rpm-ostree inherits work in [ostree-rs-ext](https://github.com/ostreedev/ostree-rs-ext/) to
create "container native ostree" functionality.  This elevates OCI/docker containers to
be natively supported as a transport mechanism for bootable operating systems.

## Rebasing a client system

Use this to switch to booting from a container image:

```
$ rpm-ostree rebase --experimental ostree-unverified-registry:quay.io/coreos-assembler/fcos:testing-devel
```

In the near future, we hope to push this more officially to `quay.io/fedora/coreos:stable`.

However, this model would just be using Docker/OCI transport "on the wire"
for content that already exists today.  This would aid things like mirroring
the OS alongside other container images, but for many users the next step
is more interesting:

## Using custom builds

The ostree container functionality supports layered container images.
See [fcos-derivation-example](https://github.com/coreos/fcos-derivation-example)
for an example.

This functionality is explicitly experimental; it is unlikely to break booting
or anything like that, but the container format or buildsystem may change.
For example, see [this issue](https://github.com/ostreedev/ostree-rs-ext/issues/159)
where we may require a command to be run as part of the build in the future.

## Creating base images

The ostree-container model creates a bidirectional bridge between ostree and OCI
formatted containers.  `rpm-ostree compose tree` today is a tool which natively
accepts RPMs (and other content) and outputs an OSTree commit.

In ostree upstream, there is a simplistic CLI (and API) that "encapsulates"
a commit into a container image with a *single layer*:

```
$ ostree container encapsulate --repo=/path/to/repo fedora/35/x86_64/silverblue docker://quay.io/myuser/fedora-silverblue:35
```

The `encapsulate` command accepts all the same "transport prefixes" as the `skopeo`
CLI.  For more information, see `man skopeo`.

However, this "single layer" is not an efficient way to deliver content.  It means
that any time anything in the ostree commit changes, clients need to download
a full new tarball.

The ostree shared library has low level APIs that support creating reproducible
"chunked" images.  A key adavantage of this is that if e.g. just the kernel
changes, one only downloads the layer containing the kernel/initramfs
(plus a metadata layer) instead of everything.

Use a command like this to generate chunked images:

```
$ rpm-ostree compose container-encapsulate --repo=/path/to/repo fedora/35/x86_64/silverblue docker://quay.io/myuser/fedora-silverblue:35
```

It is likely at some point that we add `rpm-ostree compose container` or so
which would natively input and output a (base) container image.
