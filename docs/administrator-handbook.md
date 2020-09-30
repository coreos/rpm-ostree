---
nav_order: 3
---

# Client administration
{: .no_toc }

1. TOC
{:toc}

## Administering an rpm-ostree based system

At the moment, there are four primary commands to be familiar with on
an `rpm-ostree` based system.  Also remember that in a Project Atomic
system, the `atomic host` command (from the
[Atomic command](https://github.com/projectatomic/atomic/)) is an
alias for `rpm-ostree`.

```
# rpm-ostree status
```

Will show you your deployments in the order in which they will appear in the
bootloader, the first deployment in the list being the current default one. The
`●` shows the currently booted deployment.

```
# rpm-ostree upgrade
```

Will perform a system upgrade, creating a *new* deployment (root filesystem) and
set it as the default for the next boot.  You should use `systemctl reboot`
shortly afterwards.

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
ostree [unlocking](https://github.com/ostreedev/ostree/blob/master/man/ostree-admin-unlock.xml) mechanism).

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

In order to uninstall a package that is a part of the base layer, use:

```
# rpm-ostree override remove <pkg>
```

For example:

```
# rpm-ostree override remove firefox
```

By default, every `rpm-ostree` operation is "offline" - it has no effect
on your running system, and will only take effect when you reboot.  This "pending" state is
called the "pending deployment".  Operations can be chained; for example,
if you invoke `rpm-ostree upgrade` after installing a package, your new root
will upgraded with the package also installed.

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
One of those is `rpm-ostree ex livefs`, which offers the ability to apply
changes from the pending deployment to the booted deployment.

See `man rpm-ostree` for more information.

## Filesystem layout

The only writable directories are `/etc` and `/var`.  In particular,
`/usr` has a read-only bind mount at all times.  Any data in `/var` is
never touched, and is shared across upgrades.

At upgrade time, the process takes the *new default* `/etc`, and adds
your changes on top.  This means that upgrades will receive new
default files in `/etc`, which is quite a critical feature.

For more information, see
[OSTree: Adapting](https://ostree.readthedocs.io/en/latest/manual/adapting-existing/).

## Operating system changes

 * The RPM database is stored in `/usr/share/rpm`, and is immutable.
 * A package [nss-altfiles](https://github.com/aperezdc/nss-altfiles)
   is required, and the system password database is stored in
   `/usr/lib/passwd`.  Similar for the group database.  This might
   change in the future; see
   [this issue](https://github.com/projectatomic/rpm-ostree/issues/49).
