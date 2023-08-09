---
nav_order: 5
---

# ostree native containers

rpm-ostree inherits work in [ostree-rs-ext](https://github.com/ostreedev/ostree-rs-ext/) to
create "container native ostree" functionality.  This elevates OCI/docker containers to
be natively supported as a transport mechanism for bootable operating systems.

## Rebasing a client system

Use this to switch to booting from a container image:

```
$ rpm-ostree rebase ostree-unverified-registry:quay.io/fedora/fedora-coreos:stable
```

However, this model would just be using Docker/OCI transport "on the wire"
for content that already exists today.  This would aid things like mirroring
the OS alongside other container images, but for many users the next step
is more interesting:

### Upgrading

After a rebase, all further rpm-ostree operations work as you'd expect.
For example, `rpm-ostree upgrade` will look for a new container version.
You can also `rpm-ostree apply-live`, etc.  It also does still work
to do "client side" `rpm-ostree install` etc.

## URL format for ostree native containers

Ostree understand the following URL formats to retrieve and optionally verify
the integrity of a container image or its content:

- `ostree-unverified-image:registry:<oci image>` or
  `ostree-unverified-image:docker://<oci image>`: Fetch a container image
  without verify either the integrity of the container itself not its content.
  The container image is usually fetched over HTTPS which still provides
  integrity and confidentiality but not authenticity.

- `ostree-unverified-registry:<oci image>`: Shortcut for the above use case.

- `ostree-remote-image:<ostree remote>:registry:<oci image>` &
  `ostree-remote-image:<ostree remote>:docker://<oci image>`: Fetch a container
  image and verify that the included ostree commit is correctly signed by a key
  as configured locally in the specified ostree remote
  (`/etc/ostree/remotes.d/<ostree remote>.conf`).

- `ostree-remote-registry:<ostree remote>:<oci image>`: Shortcut for the above
use case.

- `ostree-image-signed:registry:<oci image>` &
  `ostree-image-signed:docker://<oci image>`: Fetch a container image and
  verify that the container image is signed according to the policy set in
  `/etc/containers/policy.json` (see
  [containers-policy.json(5)](https://github.com/containers/image/blob/main/docs/containers-policy.json.5.md#policy-requirements)).

ostree ([ostree-rs-ext](https://github.com/ostreedev/ostree-rs-ext)) uses
[skopeo](https://github.com/containers/skopeo) to fetch container images and
thus supports the transports as documented in
[containers-transports(5)](https://github.com/containers/image/blob/main/docs/containers-transports.5.md).

## Registry authentication

Today, the ostree stack will read `/etc/ostree/auth.json` and `/run/ostree/auth.json`
which are in the same format as documented by
[containers-auth.json(5)](https://github.com/containers/image/blob/main/docs/containers-auth.json.5.md).

## Using custom builds

The ostree container functionality supports layered container images; you can
use any container buildsystem you like to add additional layers.
See [coreos-layering-examples](https://github.com/coreos/coreos-layering-examples)
many examples.  Note: The functionality here is not specific to (Fedora) CoreOS, but
it happens to be the farthest along in productizing this at the time of this writing.

This functionality is currently classified as experimental, but it is rapidly
heading to stabiliziation.

## Filesystem layout model

The ostree model defines effectively 3 partitions:

- `/usr`: Read-only (at runtime, by default) binaries and data files
- `/etc`: Mutable machine-local configuration files
- `/var`: All other state

This means that it will not currently work to install e.g. RPM packages
that add files in `/opt` by default.

### Installing packages

You can use e.g. `rpm-ostree install` to install packages.  This functions
the same as with e.g. `dnf` or `microdnf`.  It's also possible to use `rpm`
directly, e.g. `rpm -Uvh https://mirror.example.com/iptables-1.2.3.rpm`.

### Installing config files

You can use any tooling you want to generate config files in `/etc`.  When
a booted system pulls an updated container images, the changes will also
be applied.

### Installing non-RPM content

A major change compared to previous rpm-ostree is that it is now clearly
supported to install non-RPM binaries into `/usr` - these are equally
"first-class" as binaries from the base image.

### Adapting software

The way ostree works may require some changes in software.

#### Dealing with `/opt`

Some RPMs install files in `/opt`, which in the ostree model is `/var/opt`.
In  the case where the files in `/opt` are just binaries, one approach
is to move them at build time:

```
FROM quay.io/fedora/fedora-coreos:testing-devel
RUN mkdir /var/opt && \
    rpm -Uvh https://downloads.linux.hpe.com/repo/stk/rhel/7/x86_64/current/hp-scripting-tools-11.60-20.rhel7.x86_64.rpm && \
    mv /var/opt/hp/ /usr/lib/hp && \
    echo 'L /opt/hp - - - - ../../usr/lib/hp' > /usr/lib/tmpfiles.d/hp.conf && \
    ostree container commit
```

#### Users and groups

At the current time, `rpm-ostree` will auto-synthesize [systemd-sysusers](https://www.freedesktop.org/software/systemd/man/systemd-sysusers.html)
snippets when `useradd` or `groupadd` are invoked during the process of e.g. `rpm-ostree install`.

This means that user and group IDs are allocated per machine.

### Using "ostree container commit"

In a container build, it's a current best practice to invoke this at the end
of each `RUN` instruction (or equivalent).  This will verify compatibility
of `/var`, and also clean up extraneous files in e.g. `/tmp`.

In the future, this command may perform more operations.

## Creating base images

There is now an `rpm-ostree compose image` command which generates a new base image using a treefile:

```
$ rpm-ostree compose image --initialize --format=ociarchive workstation-ostree-config/fedora-silverblue.yaml fedora-silverblue.ociarchive
```

The `--initialize` command here will create a new image unconditionally.  If not provided,
the target image must exist, and will be used for change detection.  You can also directly push
to a registry:

```
$ rpm-ostree compose image --initialize --format=registry workstation-ostree-config/fedora-silverblue.yaml quay.io/example/exampleos:latest
```

## Converting OSTree commits to new base images

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

This "chunked" format is used by default by `rpm-ostree compose image`.
