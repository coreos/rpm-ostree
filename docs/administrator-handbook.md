---
nav_order: 3
---

# Client administration
{: .no_toc }

1. TOC
{:toc}

## Administering an rpm-ostree based system

At the moment, there are four primary commands to be familiar with on
an `rpm-ostree` based system.

```
# rpm-ostree status
```

Will show you your deployments in the order in which they will appear in the
bootloader, the first deployment in the list being the current default one. The
`●` shows the currently booted deployment.

```
# rpm-ostree upgrade
```

Will prepare a system upgrade offline, creating a *new* deployment (root filesystem) and
set it as the default for the next boot.  The update will be "finalized" at
shutdown and a new bootloader entry prepared.  Hence, use `reboot` to apply
the update.

```
# rpm-ostree rollback
```

This rolls back to the previous state, i.e. the default deployment changes
places with the non-default one.  By default, the `rpm-ostree upgrade` will keep
at most two bootable "deployments", though the underlying technology supports
more.


```
# rpm-ostree deploy <version>
```

This command makes use of the server-side history feature of OSTree.
It will search the history of the current branch for a commit with the
specified version, and deploy it.  This can be used in scripts to
ensure consistent updates.  For example, if the upstream OS vendor
provides an update online, you might not want to deploy it until
you've tested it.  This helps ensure that when you upgrade, you are
getting exactly what you asked for.

### Hybrid image/packaging via package layering

It is possible to dynamically add more packages onto the system that are not
part of the commit composed on the server. These additional "layered" packages
are persistent across upgrades, rebases, and deploys (contrast with the
ostree [unlocking](https://manned.org/man/fedora/ostree-admin-unlock) mechanism).

This is where the true hybrid image/package nature of rpm-ostree comes into
play; you get a combination of the benefits of images and packages.  The
package updates are still fully transactional and offline.

For example, you can use package layering to install 3rd party
kernel modules, or userspace driver daemons such as `pcsc-lite-ccid`.
While most software should go into a container, you have full flexibilty
to use packages where it suits.

```
# rpm-ostree install <pkg>
```

Will download the target package, its dependencies, and create a new deployment
with those packages installed.  It is also possible to specify a local package
which is not part of a repository.

To remove layered packages, use:

```
# rpm-ostree uninstall <pkg>
```

By default, every `rpm-ostree` operation is "offline" - it has no effect
on your running system, and will only take effect when you reboot.  This "pending" state is
called the "pending deployment".  Operations can be chained; for example,
if you invoke `rpm-ostree upgrade` after installing a package, your new root
will upgraded with the package also installed.

As a special case, it is supported to live-apply just package additions, assuming
that there are not other pending changes:

```
# rpm-ostree install -A <pkg>
```

### Modularity

rpm-ostree provides experimental support for modules, a way for the distribution
to ship multiple versions (or "streams") of the same software.

A module can have multiple streams, and each stream can have multiple profiles.
A profile is a set of packages for common use cases (e.g. you can have a
"client" and "server" profile, each installing different packages).

`rpm-ostree ex module enable` enables a module stream and allow you to
individually pick packages to `rpm-ostree install` from that stream.
`rpm-ostree ex module install` installs module stream profiles directly.

For example, to enable the `cri-o:1.20` module stream, use:

```
# rpm-ostree ex module enable cri-o:1.20
```

You can then `rpm-ostree install` individual packages from the enabled module.

Or to install a predefined profile, use e.g.:

```
# rpm-ostree ex module install cri-o:1.20/default
```

For more information about modularity, see
[the Fedora documentation](https://docs.fedoraproject.org/en-US/modularity). In
particular,
[this page](https://docs.fedoraproject.org/en-US/modularity/installing-modules/#_installing_packages)
provides sample syntax invocations.

### Rebasing

```
# rpm-ostree rebase -b $branchname
```

Your operating system vendor may provide multiple base branches.  For example,
Fedora Atomic Host has branches of the form:

  - `fedora/27/aarch64/atomic-host`
  - `fedora/27/aarch64/testing/atomic-host`
  - `fedora/27/aarch64/updates/atomic-host`
  - `fedora/27/ppc64le/atomic-host`
  - `fedora/27/ppc64le/testing/atomic-host`
  - `fedora/27/ppc64le/updates/atomic-host`
  - `fedora/27/x86_64/atomic-host`
  - `fedora/27/x86_64/testing/atomic-host`
  - `fedora/27/x86_64/updates/atomic-host`

You can use the `rebase` command to switch between these; this can represent a
major version upgrade, or logically switching between different "testing"
streams within the same release. Like every other `rpm-ostree` operation, All
layered packages and local state will be carried across.

### Other local state changes

See `man rpm-ostree` for more.  For example, there is an `rpm-ostree initramfs`
command that enables local initramfs generation.

### Experimental interface

There is a generic `rpm-ostree ex` command that offers experimental features.
One of those is `rpm-ostree ex apply-live`, which offers the ability to apply
changes from the pending deployment to the booted deployment.

See `man rpm-ostree` for more information.

## Using overrides and `usroverlay`

While some people talk about "immutability" when referring to image-based
systems like rpm-ostree, in fact a top level goal of rpm-ostree is
to *empower* users and system administrators.  When something goes
wrong, you are root on your own computer and should have the ability to apply
overrides locally.

First, there is the `rpm-ostree override replace` command, which
will replace an RPM, and apply that change persistently for the *next*
boot - this is symmetric with how `rpm-ostree install` works.

For example, suppose you want to test a fix to `podman`.  You can pass
both direct HTTP URLs as well as local files:

```
$ rpm-ostree override replace https://kojipkgs.fedoraproject.org//packages/podman/3.3.1/1.fc34/x86_64/podman-3.3.1-1.fc34.x86_64.rpm
```

### Resetting overrides

Use e.g. `rpm-ostree override reset podman` to undo the previous change.
If invoked now, nothing will have happened to the booted filesystem tree.

### Applying overrides live

Now, suppose that you want to test this change *live*.  There are two choices.
The first choice is to run the `rpm-ostree override replace` command above to stage the deployment, and then run

```
$ rpm-ostree ex apply-live --allow-replacement
```

This is a currently experimental interface that will pull the pending
changes and apply them live.  You can `rpm-ostree ex apply-live --reset`
to revert back to the booted tree.

### Using `usroverlay`

The second choice is `rpm-ostree usroverlay` which creates a transient writable `overlayfs` over `/usr` where you
can do anything, such as e.g. copying in a `podman` binary generated on a build
server somewhere that may not be in an RPM even.

The changes here will not persist across reboots, which makes this a great choice for
testing.

One downside though is there's no equivalent of `rpm-ostree ex apply-live --reset`
today when `rpm-ostree usroverlay` is in place.  It's possible to find the original
binaries in a previous deployment, or via `ostree checkout` of the base commit, etc.

### Removing a base package

You can also just simply remove a base package with `rpm-ostree override remove <pkg>`.
It will still be present in the underlying OSTree repository in `/ostree/repo`, but
it will not be visible in the generated derived commit.

Similar to the `override replace` case, using `rpm-ostree override reset` will undo
the change.

## Filesystem layout

The only writable directories are `/etc` and `/var`.  In particular,
`/usr` has a read-only bind mount at all times.  Any data in `/var` is
never touched, and is shared across upgrades.

At upgrade time, the process takes the *new default* `/etc`, and adds
your changes on top.  This means that upgrades will receive new
default files in `/etc`, which is quite a critical feature.

For more information, see
[OSTree: Adapting](https://ostreedev.github.io/ostree/adapting-existing/).

## Operating system changes

 * The RPM database is stored in `/usr/share/rpm`, and is immutable.
 * A package [nss-altfiles](https://github.com/aperezdc/nss-altfiles)
   is required, and the system password database is stored in
   `/usr/lib/passwd`.  Similar for the group database.  This might
   change in the future; see
   [this issue](https://github.com/projectatomic/rpm-ostree/issues/49).
