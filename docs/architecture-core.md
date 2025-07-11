---
parent: Architecture
nav_order: 1
---

# RPM packages, ostree commits
{: .no_toc }

1. TOC
{:toc}

## RPMs + config -> single OSTree commit

On the compose side, to generate the "base image" the core idea
is that we take a set of packages as input, along with other configuration
and data and generate a single OSTree commit - a versioned filesystem
tree.

The same is also true on the client side, but it starts from a "base commit".

This document will describe the "core" phases and steps in that
process that apply both build/compose side and client side.

## Philosophy: Every change is "from scratch"

For every change today, rpm-ostree generally rebuilds the target filesystem
"from scratch" - adding accurate caching where needed.  The goal is to
avoid [hysteresis](https://en.wikipedia.org/wiki/Hysteresis) ([related blog](https://blog.verbum.org/2020/08/22/immutable-%E2%86%92-reprovisionable-anti-hysteresis/)).

In other words if e.g. you `rpm-ostree install foo` and then `rpm-ostree install bar`,
the new target filesystem tree will be regenerated "from scratch" and
all RPM `%post` scripts etc. will rerun.

## Base vs extensions split

rpm-ostree today goes to some lengths to create a distinction between
the "base image" and optional "layered/extension" packages.  The
idea is that the base image has been tested on a build server, and
any overrides or replacements are more likely to break things, and
should be explicit actions.

This is the reason why on the client side e.g. `rpm-ostree upgrade kernel`
does not work today.  Instead, this requires an invocation of
`rpm-ostree override replace`.

Another related rationale for this is that rpm-ostree does not
have a per-package "user installed" type database as exists in e.g. `dnf`.
Everything in the base image is always replaced by the contents of the
new base image, without considering any per-package semantics.
For more on this topic, see [this blog entry](https://blog.verbum.org/2020/08/22/immutable-%E2%86%92-reprovisionable-anti-hysteresis/).

In some cases, a dependency may "span" the base image and a layered package.
For discussion related to this, see e.g. https://github.com/coreos/rpm-ostree/issues/415
A solution chosen for in Fedora is to ensure that such packages have
versions corresponding to the base image available in the rpm-md repository
as well; this is implemented via the [updates-archive repository](https://src.fedoraproject.org/rpms/fedora-repos/blob/rawhide/f/fedora-updates-archive.repo).

## Overall architecture:

- For each package, download and import into OSTree commit if necessary
- Unpack the "base" filesystem tree if any via hardlinks
- Determine an installation order, and unpack each package-ostree commit
  again via hardlinks
- Run all the `%post` scripts (in install order)
- Run all the `%posttrans` scripts (in install order)
- Write RPM database (if we had a "base commit", starting from that)
- If initramfs regeneration is enabled or the kernel was replaced,
  remove the base initramfs and run `dracut` to generate a new one.
- Ask libostree to commit the resulting filesystem tree, optimized
  by a (device, inode) -> checksum cache, so that files that weren't
  changed aren't re-checksummed.

### Generating the filesystem tree

In contrast to the above, traditional package managers like RPM are usually implemented in
a flow that does:

- Unpack package A
- Run `%post` script for A
- Update the package database with metadata for A
- Unpack package B
- Run `%post` script for B
- Update the package database with metadata for B
- ...
- Run `%posttrans` scripts

etc.

In contrast, rpm-ostree maintains an OSTree commit corresponding
to each RPM package provided as input.  On a client system,
you can see this in e.g. `ostree refs | grep rpmostree/pkg` 
(assuming you have layered packages).  On a build system,
these ostree commits will be stored in a repo at
`pkgcache-repo/` within the cache directory.

This acts as an optimized cache for regenerating the target
root filesystem.  So for rpm-ostree, the phase is more like this:

- Unpack the filesystem tree for all packages
- Run all the `%post` scripts
- Run all the `%posttrans` scripts
...

rpm-ostree is effectively reimplementing large chunks of
the librpm userspace in order to make it use OSTree natively.

### Sandboxing scripts

On the build server side, it's obviously desirable to 
have the "build" of an ostree commit for a target system
not affect the running host.

Similarly, on the client side, the default is to provide
"offline" updates that don't affect the running system.

As part of this, rpm-ostree currently uses the
[bubblewrap](https://github.com/containers/bubblewrap/)
tool to run each script in its own isolated container.


Today, scripts are run with real uid 0 (not in a user namespace),
but we [drop most capabilities](https://github.com/coreos/rpm-ostree/pull/1099).
Additionally, scripts can't see the real host root filesystem,
most notably they do not see the real `/var` with all of the
system data.  A good example of the benefit of this is
["tests: Add a test case for a %post that does rm -rf /"](https://github.com/coreos/rpm-ostree/pull/888).

In addition to bubblewrap, rpm-ostree uses `rofiles-fuse`
from the ostree project which originally enforced the model that
a file that has multiple hardlinks is read-only, but
more recently gained `--copyup` support which acts
in a similar fashion to the in-kernel `overlayfs`.
(See also https://github.com/ostreedev/ostree/issues/2281)

Scripts can mutate content in `/etc` and `/usr`, but this should
generally be restricted to updating "cache files".  Example of this
are `ldconfig` and `gtk-update-icon-cache`.

Scripts see a minimal, read-only view of `/var`.  It is explicitly
not writable.  This helps ensure offline updates are possible; a
package script running client side cannot affect persistent system
state.

#### Lua scripts

At the moment, because lua scripts aren't sandboxed they are not supported.
For more information, see <https://github.com/coreos/rpm-ostree/issues/749>.

However, in most cases these scripts are "upgrade hacks" or would mutate
live system state, and should generally just be skipped. If you want
these scripts to only apply to librpm systems (e.g. `dnf` but not `rpm-ostree`)
then you can add a magic comment in the first 10 lines:

```
-- rpm-ostree-skip
```

This will cause rpm-ostree to ignore the script.

### Content in /var

rpm-ostree automatically synthesizes [systemd tmpfiles.d](https://www.freedesktop.org/software/systemd/man/tmpfiles.d.html)
snippets from directories in `/var`.  This helps ensure a separation
between OS updates and machine local state.  The directories
will be created when the system boots - and it aids in a "factory reset"
scenario of wiping machine-local state in `/var`.

At the current time, any non-directories (i.e. regular files or symbolic links)
are discarded.  This will likely change to be an opt-in error in the future.

Example bugs:

 - https://bugzilla.redhat.com/show_bug.cgi?id=2132103
 - https://bugzilla.redhat.com/show_bug.cgi?id=1473402

### Kernel handling

ostree is entirely oriented around bootable filesystem trees;
its "source of truth" is the bootloader entries.  It has opinions
about where the Linux kernel binaries are stored (the current
standard is in `/usr/lib/modules/$kver`.)

In contrast, traditional RPM is unaware of what a kernel is; it's
just another package.  Most higher level package managers such
as yum gained some special casing around the kernel - because
it's not possible to restart the running kernel, traditional RPM
systems need to keep the kernel modules for the running kernel
around.  For example yum/dnf have a concept of "installonlyn"
which defaults to 2 for the kernel package.

Additionally, for at least traditional Fedora derivatives with
yum/dnf, the initramfs is generated client side as part of
a kernel update.

But for rpm-ostree, the decision was made to default to a
pre-generated initramfs by default.  Further, in order to implement transactional
upgrades, rpm-ostree needs to be in control of the initramfs
regeneration - it can't just be a script forked off without its
knowledge.

Further for rpm-ostree, easily replacing the kernel (as well as userspace)
is intended to be a first-class operation; you need to be able to do that
in order to debug production issues for example.

In contrast to the yum/dnf "installonly" for ostree there can be exactly one kernel per userspace
filesystem tree.  To ostree, a "bootable ostree commit"
is the pair of (kernel, userspace).

rpm-ostree combines these two worlds, and goes to some
lengths to bend the libdnf stack to work this way.  We reset
the "installonly" limit back to 1 to ensure we have exactly
one kernel.  PR: https://github.com/coreos/rpm-ostree/pull/1228

Further, as noted above rpm-ostree takes over the handling of
invoking `dracut` - just like other scripts, it is run inside
a container with just read-only access to the system.  `dracut`
generates the initramfs CPIO archive, which we then place inside
the `/usr/lib/modules/$kver` location.

If client-side initramfs regeneration is enabled, we may selectively
provide desired configuration files into this process.  PR: https://github.com/coreos/rpm-ostree/pull/2170

### SELinux

Handling SELinux is very tricky, because it is a package that can affect
*every other package*.  Specifically, the SELinux policy package
contains a vast set of regular expressions in `file_contexts`
to determine labeling.

For traditional librpm, this is a plugin.

A major goal of OSTree from the start has been to ensure fully correct
handling of SELinux for the base operating system.  The way
rpm-ostree handles this is by:

- Recompiling the policy as a `%posttrans` equivalent
- Loading the policy from the target root, and pass that loaded policy
  to libostree, which consults it to use for the label of each
  committed file.

This means that on an OSTree based system, the labels for the
files in the booted deployment (e.g. in `/usr`) are always
correct and set atomically - there's no need to relabel.
#### SELinux policy storage location

Another major difference between traditional yum/dnf and
rpm-ostree based systems is the location of the SELinux
policy store database itself.  rpm-ostree overrides it
to be back in `/etc`, when it was moved to `/var` in the
RPM package around the Fedora 24 timeframe.  For more information
see https://bugzilla.redhat.com/show_bug.cgi?id=1290659 
and the comments in `rpmostree-postprocess.cxx`.
