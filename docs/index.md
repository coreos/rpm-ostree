---
nav_order: 1
---

# A true hybrid image/package system
{: .no_toc }

1. TOC
{:toc}

rpm-ostree is a hybrid image/package system.  It combines
[libostree](https://ostreedev.github.io/ostree/) as a base image format,
and accepts RPM on both the client and server side, sharing code with the
[dnf](https://en.wikipedia.org/wiki/DNF_(software)) project; specifically
[libdnf](https://github.com/rpm-software-management/libdnf). and thus bringing
many of the benefits of both together.

```
                         +-----------------------------------------+
                         |                                         |
                         |       rpm-ostree (daemon + CLI)         |
                  +------>                                         <---------+
                  |      |     status, upgrade, rollback,          |         |
                  |      |     pkg layering, initramfs --enable    |         |
                  |      |                                         |         |
                  |      +-----------------------------------------+         |
                  |                                                          |
                  |                                                          |
                  |                                                          |
+-----------------|-------------------------+        +-----------------------|-----------------+
|                                           |        |                                         |
|         libostree (image system)          |        |            libdnf (pkg system)          |
|                                           |        |                                         |
|   C API, hardlink fs trees, system repo,  |        |    ties together libsolv (SAT solver)   |
|   commits, atomic bootloader swap         |        |    with librepo (RPM repo downloads)    |
|                                           |        |                                         |
+-------------------------------------------+        +-----------------------------------------+
```

**Features:**

 - Transactional, background image-based (versioned/checksummed) upgrades
 - OS rollback without affecting user data (`/usr`, `/etc` but not `/var`) via libostree
 - Client-side package layering (and overrides)
 - Easily make your own derivatives

## Projects using rpm-ostree

The OSTree project is independent of distributions and agnostic to how content
is delivered and managed; it's used today by e.g. Debian, Fedora, and
OpenEmbedded derived systems among others. There are some examples in the
[OSTree github](https://github.com/ostreedev/ostree).

In contrast, rpm-ostree is intended to be tightly integrated with the Fedora
ecosystem. Today it is the underlying update mechanism of
[Fedora CoreOS](https://getfedora.org/coreos/) as well as its derivative RHEL
CoreOS. It is also used by [Fedora IoT](https://iot.fedoraproject.org/) and
[Fedora Silverblue](https://silverblue.fedoraproject.org/).

Originally, it was productized as part of [Project Atomic](http://www.projectatomic.io/).

## Getting started

If you want to try the system as a user, see the main [Fedora website](https://getfedora.org/)
which has several versions that use rpm-ostree, including Silverblue, IoT and CoreOS.
If you are interested in assembling your own systems, see [compose server](compose-server.md).
## Why?

Package systems such as apt and yum are highly prevalent in Linux-based
operating systems. They offer a lot of flexiblity, but have many failure
modes.

The core premise of rpm-ostree is that offline transactional image-based updates
should be the default.  This provides a high degree of predictability and
resiliency.  The operating system vendor can focus a lot of effort
on testing the "base images" as a unit.

Further, image based updates simply work better at scale.  For "IoT" style devices
it's very inefficient to have each machine perform dependency resolution,
run package scripts etc.  And the same is true for many server datacenter
use cases.

Where rpm-ostree is fairly unique in the ecosystem is
supporting client-side package layering and overrides; deeply integrating RPM
as an (optional) layer on top of OSTree.

A good way to think of package layering is recasting RPMs as "operating system
extensions", similar to how browser extensions work (although before those were
sandboxed). One can use package layering for components not easily
containerized, such as PAM modules, custom shells, etc.

Further, one can easily use `rpm-ostree override replace` to override the
kernel or userspace components with the very same RPMs shipped to traditional
systems. The Fedora project for example continues to only have one kernel
build.

Layering and overrides are still built on top of the default OSTree engine -
installing and updating client-side packages constructs a new filesystem root,
it does not by default affect your booted root. This preserves the "image"
nature of the system.

By its nature as a hybrid image/package system, rpm-ostree is intended
to span nearly all use cases of current package systems *and* image
systems.

## Why would I want to use it?

One major feature rpm-ostree has over traditional package management
is atomic upgrade/rollback.  It supports a model where an OS vendor
(such as [CentOS](https://www.centos.org/) or
[Fedora](https://getfedora.org/)) can provide pre-assembled "base OS
images", and client systems can replicate those, and possibly layer on
additional packages.

## Why not implement these changes in an existing package manager?

The [OSTree related projects](https://coreos.github.io/ostree/related-projects/)
section covers this to a degree.  As soon as one starts taking
"snapshots" or keeping track of multiple roots, it uncovers many
issues.  For example, which content specifically is rolled forward or
backwards?  If the package manager isn't deeply aware of a snapshot
tool, it's easy to lose coherency.

### Filesystem layout

A concrete example is that rpm-ostree moves the RPM database
to `/usr/share/rpm`, since we want one per root `/usr`.  In contrast,
the [snapper](http://snapper.io/) tool goes to some effort to
include `/var/lib/rpm` in snapshots, but
avoid rolling forward/back log files in `/var/log`.

OSTree requires clear rules around the semantics
of directories like `/usr` and `/var` across upgrades, and
while this requires changing some software, we believe the
result is significantly more reliable than having two separate
systems like yum and snapper glued together, or apt-get and BTRFS,
etc.

### User experience

Furthermore, beyond just the mechanics of things like the filesystem
layout, the implemented upgrade model affects the entire user
experience.

For example, the base system OSTree commits that one replicates from a
remote server can be assigned version numbers.  They are
released as coherent wholes, tested together.  If one is simply
performing snapshots on the client side, every client machine
can have different versions of components.

Related to this is that rpm-ostree clearly distinguishes which
packages you have layered, and it's easy to remove them, going back to
a pristine, known state.  Many package managers just implement a "bag
of packages" model with no clear bases or layering.  As the OS evolves
over time, "package drift" occurs where you might have old, unused
packages lying around.

## But still evolutionary

On the other hand, rpm-ostree in other ways is very evolutionary.
There have been many, many different package managers invented -
why not adopt or build on one of those?

The answer here is that it takes a long time for tooling to be built
on top of a package format - things like mirroring servers.  Another
example is source format representations - there are many, many
tools that know how to build source RPMs.

From the perspective of distribution which has all of that ecosystem
built up, rpm-ostree does introduce a new binary format (ostree), but
otherwise includes an RPM database, and also operates on packages.  It
is not a new source format either.

## Talks and media

A number of Project Atomic talks are available; see for
example [this post](https://lists.projectatomic.io/projectatomic-archives/atomic-devel/2018-January/msg00057.html)
which has a bigger collection that also includes talks on containers.

rpm-ostree specific talks:

 * devconf.cz 2018:
   [Colin Walters: Hybrid image/package OS updates with rpm-ostree](https://www.youtube.com/watch?v=4A_xl5dC210)
   ([slides](https://fedorapeople.org/~walters/2018.01-devconf/index.html))
 * devconf.cz 2018:
   [Peter Robinson: Using Fedora and OSTree for IoT](https://www.youtube.com/watch?v=mRqV38qT-wc)

## License

rpm-ostree includes code licensed under GPLv2+, LGPLv2+, (Apache 2.0 OR MIT).
For more information, see [LICENSE](https://github.com/coreos/rpm-ostree/blob/main/LICENSE).
